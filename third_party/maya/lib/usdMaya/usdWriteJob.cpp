//
// Copyright 2016 Pixar
//
// Licensed under the Apache License, Version 2.0 (the "Apache License")
// with the following modification; you may not use this file except in
// compliance with the Apache License and the following modification to it:
// Section 6. Trademarks. is deleted and replaced with:
//
// 6. Trademarks. This License does not grant permission to use the trade
//    names, trademarks, service marks, or product names of the Licensor
//    and its affiliates, except as required to comply with Section 4(c) of
//    the License and to reproduce the content of the NOTICE file.
//
// You may obtain a copy of the Apache License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the Apache License with the above modification is
// distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied. See the Apache License for the specific
// language governing permissions and limitations under the Apache License.
//
#include "pxr/pxr.h"
#include "usdMaya/usdWriteJob.h"

#include "usdMaya/jobArgs.h"
#include "usdMaya/MayaPrimWriter.h"
#include "usdMaya/MayaTransformWriter.h"

#include "usdMaya/translatorMaterial.h"
#include "usdMaya/primWriterRegistry.h"
#include "usdMaya/shadingModeExporterContext.h"

#include "usdMaya/chaser.h"
#include "usdMaya/chaserRegistry.h"

#include "pxr/usd/ar/resolver.h"
#include "pxr/usd/usd/modelAPI.h"
#include "pxr/usd/kind/registry.h"

#include "pxr/usd/usd/variantSets.h"
#include "pxr/usd/usd/editContext.h"
#include "pxr/usd/usd/primRange.h"
#include "pxr/usd/usdGeom/metrics.h"
#include "pxr/usd/usdGeom/xform.h"
#include "pxr/usd/usdUtils/pipeline.h"
#include "pxr/base/tf/hash.h"
#include "pxr/base/tf/hashset.h"
#include "pxr/base/tf/stl.h"

#include "pxr/usd/sdf/layer.h"
#include "pxr/usd/sdf/primSpec.h"
// Needed for directly removing a UsdVariant via Sdf
//   Remove when UsdVariantSet::RemoveVariant() is exposed
//   XXX [bug 75864]
#include "pxr/usd/sdf/variantSetSpec.h"
#include "pxr/usd/sdf/variantSpec.h"

#include <maya/MFnDagNode.h>
#include <maya/MFnRenderLayer.h>
#include <maya/MGlobal.h>
#include <maya/MItDag.h>
#include <maya/MObjectArray.h>
#include <maya/MPxNode.h>
#include <maya/MStatus.h>

#include <limits>
#include <map>
#include <unordered_set>

PXR_NAMESPACE_OPEN_SCOPE


usdWriteJob::usdWriteJob(const PxrUsdMayaJobExportArgs & iArgs) :
    mModelKindWriter(iArgs), mJobCtx(iArgs)
{
}


usdWriteJob::~usdWriteJob()
{
}

bool usdWriteJob::beginJob(const std::string &iFileName, bool append)
{
    // Check for DAG nodes that are a child of an already specified DAG node to export
    // if that's the case, report the issue and skip the export
    PxrUsdMayaUtil::MDagPathSet::const_iterator m, n;
    PxrUsdMayaUtil::MDagPathSet::const_iterator endPath = mJobCtx.mArgs.dagPaths.end();
    for (m = mJobCtx.mArgs.dagPaths.begin(); m != endPath; ) {
        MDagPath path1 = *m; m++;
        for (n = m; n != endPath; n++) {
            MDagPath path2 = *n;
            if (PxrUsdMayaUtil::isAncestorDescendentRelationship(path1,path2)) {
                TF_RUNTIME_ERROR(
                        "%s and %s are ancestors or descendants of each other. "
                        "Please specify export DAG paths that don't overlap. "
                        "Exiting.",
                        path1.fullPathName().asChar(),
                        path2.fullPathName().asChar());
                return false;
            }
        }  // for n
    }  // for m

    // Make sure the file name is a valid one with a proper USD extension.
    const std::string iFileExtension = TfStringGetSuffix(iFileName, '.');
    if (SdfLayer::IsAnonymousLayerIdentifier(iFileName) ||
            iFileExtension == PxrUsdMayaTranslatorTokens->UsdFileExtensionDefault ||
            iFileExtension == PxrUsdMayaTranslatorTokens->UsdFileExtensionASCII ||
            iFileExtension == PxrUsdMayaTranslatorTokens->UsdFileExtensionCrate) {
        mFileName = iFileName;
    } else {
        mFileName = TfStringPrintf("%s.%s",
                                   iFileName.c_str(),
                                   PxrUsdMayaTranslatorTokens->UsdFileExtensionDefault.GetText());
    }

    TF_STATUS("Creating stage file '%s'", mFileName.c_str());

    if (mJobCtx.mArgs.renderLayerMode == PxrUsdExportJobArgsTokens->modelingVariant) {
        // Handle usdModelRootOverridePath for USD Variants
        MFnRenderLayer::listAllRenderLayers(mRenderLayerObjs);
        if (mRenderLayerObjs.length() > 1) {
            mJobCtx.mArgs.usdModelRootOverridePath = SdfPath("/_BaseModel_");
        }
    }

    if (!mJobCtx.openFile(mFileName, append)) {
        return false;
    }

    // Set time range for the USD file if we're exporting animation.
    if (!mJobCtx.mArgs.timeInterval.IsEmpty()) {
        mJobCtx.mStage->SetStartTimeCode(mJobCtx.mArgs.timeInterval.GetMin());
        mJobCtx.mStage->SetEndTimeCode(mJobCtx.mArgs.timeInterval.GetMax());
    }

    mModelKindWriter.Reset();

    // Setup the requested render layer mode:
    //     defaultLayer    - Switch to the default render layer before exporting,
    //                       then switch back afterwards (no layer switching if
    //                       the current layer IS the default layer).
    //     currentLayer    - No layer switching before or after exporting. Just
    //                       use whatever is the current render layer for export.
    //     modelingVariant - Switch to the default render layer before exporting,
    //                       and export each render layer in the scene as a
    //                       modeling variant, then switch back afterwards (no
    //                       layer switching if the current layer IS the default
    //                       layer). The default layer will be made the default
    //                       modeling variant.
    MFnRenderLayer currentLayer(MFnRenderLayer::currentLayer());
    mCurrentRenderLayerName = currentLayer.name();

    // Switch to the default render layer unless the renderLayerMode is
    // 'currentLayer', or the default layer is already the current layer.
    if (mJobCtx.mArgs.renderLayerMode != PxrUsdExportJobArgsTokens->currentLayer &&
            MFnRenderLayer::currentLayer() != MFnRenderLayer::defaultRenderLayer()) {
        // Set the RenderLayer to the default render layer
        MFnRenderLayer defaultLayer(MFnRenderLayer::defaultRenderLayer());
        MGlobal::executeCommand(MString("editRenderLayerGlobals -currentRenderLayer ")+
                                        defaultLayer.name(), false, false);
    }

    // Pre-process the argument dagPath path names into two sets. One set
    // contains just the arg dagPaths, and the other contains all parents of
    // arg dagPaths all the way up to the world root. Partial path names are
    // enough because Maya guarantees them to still be unique, and they require
    // less work to hash and compare than full path names.
    TfHashSet<std::string, TfHash> argDagPaths;
    TfHashSet<std::string, TfHash> argDagPathParents;
    PxrUsdMayaUtil::MDagPathSet::const_iterator end = mJobCtx.mArgs.dagPaths.end();
    for (PxrUsdMayaUtil::MDagPathSet::const_iterator it = mJobCtx.mArgs.dagPaths.begin();
            it != end; ++it) {
        MDagPath curDagPath = *it;
        MStatus status;
        bool curDagPathIsValid = curDagPath.isValid(&status);
        if (status != MS::kSuccess || !curDagPathIsValid) {
            continue;
        }

        std::string curDagPathStr(curDagPath.partialPathName(&status).asChar());
        if (status != MS::kSuccess) {
            continue;
        }

        argDagPaths.insert(curDagPathStr);

        status = curDagPath.pop();
        if (status != MS::kSuccess) {
            continue;
        }
        curDagPathIsValid = curDagPath.isValid(&status);

        while (status == MS::kSuccess && curDagPathIsValid) {
            curDagPathStr = curDagPath.partialPathName(&status).asChar();
            if (status != MS::kSuccess) {
                break;
            }

            if (argDagPathParents.find(curDagPathStr) != argDagPathParents.end()) {
                // We've already traversed up from this path.
                break;
            }
            argDagPathParents.insert(curDagPathStr);

            status = curDagPath.pop();
            if (status != MS::kSuccess) {
                break;
            }
            curDagPathIsValid = curDagPath.isValid(&status);
        }
    }

    // Now do a depth-first traversal of the Maya DAG from the world root.
    // We keep a reference to arg dagPaths as we encounter them.
    MDagPath curLeafDagPath;
    for (MItDag itDag(MItDag::kDepthFirst, MFn::kInvalid); !itDag.isDone(); itDag.next()) {
        MDagPath curDagPath;
        itDag.getPath(curDagPath);
        std::string curDagPathStr(curDagPath.partialPathName().asChar());

        if (argDagPathParents.find(curDagPathStr) != argDagPathParents.end()) {
            // This dagPath is a parent of one of the arg dagPaths. It should
            // be included in the export, but not necessarily all of its
            // children should be, so we continue to traverse down.
        } else if (argDagPaths.find(curDagPathStr) != argDagPaths.end()) {
            // This dagPath IS one of the arg dagPaths. It AND all of its
            // children should be included in the export.
            curLeafDagPath = curDagPath;
        } else if (!MFnDagNode(curDagPath).hasParent(curLeafDagPath.node())) {
            // This dagPath is not a child of one of the arg dagPaths, so prune
            // it and everything below it from the traversal.
            itDag.prune();
            continue;
        }

        if (!needToTraverse(curDagPath) &&
            curDagPath.length() > 0) {
            // This dagPath and all of its children should be pruned.
            itDag.prune();
        } else {
            MayaPrimWriterSharedPtr primWriter = mJobCtx.CreatePrimWriter(curDagPath);

            if (primWriter) {
                mJobCtx.mMayaPrimWriterList.push_back(primWriter);

                // Write out data (non-animated/default values).
                if (const auto& usdPrim = primWriter->GetUsdPrim()) {
                    if (mJobCtx.mArgs.stripNamespaces) {
                        auto foundPair = mUsdPathToDagPathMap.find(usdPrim.GetPath());
                        if (foundPair != mUsdPathToDagPathMap.end()){
                            TF_RUNTIME_ERROR(
                                    "Multiple dag nodes map to the same prim "
                                    "path after stripping namespaces: %s - %s",
                                    foundPair->second.fullPathName().asChar(),
                                    primWriter->GetDagPath().fullPathName()
                                        .asChar());
                            return false;
                        }
                        // Note that mUsdPathToDagPathMap is _only_ used for
                        // stripping namespaces, so we only need to populate it
                        // when stripping namespaces. (This is different from
                        // mDagPathToUsdPathMap!)
                        mUsdPathToDagPathMap[usdPrim.GetPath()] =
                                primWriter->GetDagPath();
                    }

                    primWriter->Write(UsdTimeCode::Default());

                    const PxrUsdMayaUtil::MDagPathMap<SdfPath>& mapping =
                            primWriter->GetDagToUsdPathMapping();
                    mDagPathToUsdPathMap.insert(mapping.begin(), mapping.end());

                    mModelKindWriter.OnWritePrim(usdPrim, primWriter);
                }

                if (primWriter->ShouldPruneChildren()) {
                    itDag.prune();
                }
            }
        }
    }

    PxrUsdMayaExportParams exportParams;
    exportParams.mergeTransformAndShape = mJobCtx.mArgs.mergeTransformAndShape;
    exportParams.exportCollectionBasedBindings =
            mJobCtx.mArgs.exportCollectionBasedBindings;
    exportParams.stripNamespaces = mJobCtx.mArgs.stripNamespaces;
    exportParams.overrideRootPath = mJobCtx.mArgs.usdModelRootOverridePath;
    exportParams.bindableRoots = mJobCtx.mArgs.dagPaths;
    exportParams.parentScope = mJobCtx.mArgs.parentScope;

    // Writing Materials/Shading
    exportParams.materialCollectionsPath =
            mJobCtx.mArgs.exportMaterialCollections ?
            mJobCtx.mArgs.materialCollectionsPath :
            SdfPath::EmptyPath();

    PxrUsdMayaTranslatorMaterial::ExportShadingEngines(
                mJobCtx.mStage,
                mJobCtx.mArgs.shadingMode,
                mDagPathToUsdPathMap,
                exportParams);

    // We shouldn't be creating new instance masters after this point, and we
    // want to cleanup the InstanceSources prim before writing model hierarchy.
    mJobCtx.processInstances();

    if (!mModelKindWriter.MakeModelHierarchy(mJobCtx.mStage)) {
        return false;
    }

    if (!mJobCtx.getSkelBindingsWriter().PostProcessSkelBindings(
            mJobCtx.mStage)) {
        return false;
    }

    // now we populate the chasers and run export default
    mChasers.clear();
    PxrUsdMayaChaserRegistry::FactoryContext ctx(mJobCtx.mStage, mDagPathToUsdPathMap, mJobCtx.mArgs);
    for (const std::string& chaserName : mJobCtx.mArgs.chaserNames) {
        if (PxrUsdMayaChaserRefPtr fn =
                PxrUsdMayaChaserRegistry::GetInstance().Create(chaserName, ctx)) {
            mChasers.push_back(fn);
        }
        else {
            TF_RUNTIME_ERROR("Failed to create chaser: %s", chaserName.c_str());
        }
    }

    for (const PxrUsdMayaChaserRefPtr& chaser : mChasers) {
        if (!chaser->ExportDefault()) {
            return false;
        }
    }

    return true;
}

void usdWriteJob::evalJob(double iFrame)
{
    const UsdTimeCode usdTime(iFrame);

    for (const MayaPrimWriterSharedPtr& primWriter : mJobCtx.mMayaPrimWriterList) {
        const UsdPrim& usdPrim = primWriter->GetUsdPrim();
        if (usdPrim) {
            primWriter->Write(usdTime);
        }
    }

    for (PxrUsdMayaChaserRefPtr& chaser : mChasers) {
        chaser->ExportFrame(iFrame);
    }

    perFrameCallback(iFrame);
}

void usdWriteJob::endJob()
{
    UsdPrimSiblingRange usdRootPrims = mJobCtx.mStage->GetPseudoRoot().GetChildren();

    // Write Variants (to first root prim path)
    UsdPrim usdRootPrim;
    TfToken defaultPrim;

    if (!usdRootPrims.empty()) {
        usdRootPrim = *usdRootPrims.begin();
        defaultPrim = usdRootPrim.GetName();
    }

    if (usdRootPrim && mRenderLayerObjs.length() > 1 &&
        !mJobCtx.mArgs.usdModelRootOverridePath.IsEmpty()) {
            // Get RenderLayers
            //   mArgs.usdModelRootOverridePath:
            //     Require mArgs.usdModelRootOverridePath to be set so that
            //     the variants are put under a UsdPrim that references a BaseModel
            //     prim that has all of the geometry, transforms, and other details.
            //     This needs to be done since "local" values have stronger precedence
            //     than "variant" values, but "referencing" will cause the variant values
            //     to take precedence.
        defaultPrim = writeVariants(usdRootPrim);
    }

    // Restoring the currentRenderLayer
    MFnRenderLayer currentLayer(MFnRenderLayer::currentLayer());
    if (currentLayer.name() != mCurrentRenderLayerName) {
        MGlobal::executeCommand(MString("editRenderLayerGlobals -currentRenderLayer ")+
                                        mCurrentRenderLayerName, false, false);
    }

    postCallback();

    // Unfortunately, MGlobal::isZAxisUp() is merely session state that does
    // not get recorded in Maya files, so we cannot rely on it being set
    // properly.  Since "Y" is the more common upAxis, we'll just use
    // isZAxisUp as an override to whatever our pipeline is configured for.
    TfToken upAxis = UsdGeomGetFallbackUpAxis();
    if (MGlobal::isZAxisUp()){
        upAxis = UsdGeomTokens->z;
    }
    UsdGeomSetStageUpAxis(mJobCtx.mStage, upAxis);
    if (usdRootPrim){
        // We have already decided above that 'usdRootPrim' is the important
        // prim for the export... usdVariantRootPrimPath
        mJobCtx.mStage->GetRootLayer()->SetDefaultPrim(defaultPrim);
    }
    // Running post export function on all the prim writers.
    for (auto& primWriter: mJobCtx.mMayaPrimWriterList) {
        primWriter->PostExport();
    }
    if (mJobCtx.mStage->GetRootLayer()->PermissionToSave()) {
        mJobCtx.mStage->GetRootLayer()->Save();
    }
    mJobCtx.mStage = UsdStageRefPtr();
    mJobCtx.mMayaPrimWriterList.clear(); // clear this so that no stage references are left around
    TF_STATUS("Saving stage");
}

TfToken usdWriteJob::writeVariants(const UsdPrim &usdRootPrim)
{
    // Some notes about the expected structure that this function will create:

    // Suppose we have a maya scene, that, with no parentScope path, and
    // without renderLayerMode='modelingVariant', would give these prims:
    //
    //  /mayaRoot
    //  /mayaRoot/Geom
    //  /mayaRoot/Geom/Cube1
    //  /mayaRoot/Geom/Cube2
    //
    // If you have parentScope='foo', you would instead get:
    //
    //  /foo/mayaRoot
    //  /foo/mayaRoot/Geom
    //  /foo/mayaRoot/Geom/Cube1
    //  /foo/mayaRoot/Geom/Cube2
    //
    // If you have renderLayerMode='modelingVariant', and no parent scope, you
    // will have:
    //
    //  /_BaseModel_
    //  /_BaseModel_/Geom
    //  /_BaseModel_/Geom/Cube1
    //  /_BaseModel_/Geom/Cube2
    //
    //  /mayaRoot [reference to => /_BaseModel_]
    //     [variants w/ render layer overrides]
    //
    // If you have both parentScope='foo' and renderLayerMode='modelingVariant',
    // then you will get:
    //
    //  /_BaseModel_
    //  /_BaseModel_/mayaRoot
    //  /_BaseModel_/mayaRoot/Geom
    //  /_BaseModel_/mayaRoot/Geom/Cube1
    //  /_BaseModel_/mayaRoot/Geom/Cube2
    //
    //  /foo [reference to => /_BaseModel_]
    //     [variants w/ render layer overrides]

    // Init parameters for filtering and setting the active variant
    std::string defaultModelingVariant;

    SdfPath usdVariantRootPrimPath;
    if (mJobCtx.mParentScopePath.IsEmpty()) {
        // Get the usdVariantRootPrimPath (optionally filter by renderLayer prefix)
        MayaPrimWriterSharedPtr firstPrimWriterPtr = *mJobCtx.mMayaPrimWriterList.begin();
        std::string firstPrimWriterPathStr( firstPrimWriterPtr->GetDagPath().fullPathName().asChar() );
        std::replace( firstPrimWriterPathStr.begin(), firstPrimWriterPathStr.end(), '|', '/');
        std::replace( firstPrimWriterPathStr.begin(), firstPrimWriterPathStr.end(), ':', '_'); // replace namespace ":" with "_"
        usdVariantRootPrimPath = SdfPath(firstPrimWriterPathStr).GetPrefixes()[0];
    }
    else {
        // If they passed a parentScope, then use that for our new top-level
        // variant-switcher prim
        usdVariantRootPrimPath = mJobCtx.mParentScopePath;
    }

    // Create a new usdVariantRootPrim and reference the Base Model UsdRootPrim
    //   This is done for reasons as described above under mArgs.usdModelRootOverridePath
    UsdPrim usdVariantRootPrim = mJobCtx.mStage->DefinePrim(usdVariantRootPrimPath);
    TfToken defaultPrim = usdVariantRootPrim.GetName();
    usdVariantRootPrim.GetReferences().AddInternalReference(usdRootPrim.GetPath());
    usdVariantRootPrim.SetActive(true);
    usdRootPrim.SetActive(false);

    // Loop over all the renderLayers
    for (unsigned int ir=0; ir < mRenderLayerObjs.length(); ++ir) {
        SdfPathTable<bool> tableOfActivePaths;
        MFnRenderLayer renderLayerFn( mRenderLayerObjs[ir] );
        MString renderLayerName = renderLayerFn.name();
        std::string variantName(renderLayerName.asChar());
        // Determine default variant. Currently unsupported
        //MPlug renderLayerDisplayOrderPlug = renderLayerFn.findPlug("displayOrder", true);
        //int renderLayerDisplayOrder = renderLayerDisplayOrderPlug.asShort();

        // The Maya default RenderLayer is also the default modeling variant
        if (mRenderLayerObjs[ir] == MFnRenderLayer::defaultRenderLayer()) {
            defaultModelingVariant=variantName;
        }

        // Make the renderlayer being looped the current one
        MGlobal::executeCommand(MString("editRenderLayerGlobals -currentRenderLayer ")+
                                        renderLayerName, false, false);

        // == ModelingVariants ==
        // Identify prims to activate
        // Put prims and parent prims in a SdfPathTable
        // Then use that membership to determine if a prim should be Active.
        // It has to be done this way since SetActive(false) disables access to all child prims.
        MObjectArray renderLayerMemberObjs;
        renderLayerFn.listMembers(renderLayerMemberObjs);
        std::vector< SdfPath > activePaths;
        for (unsigned int im=0; im < renderLayerMemberObjs.length(); ++im) {
            MFnDagNode dagFn(renderLayerMemberObjs[im]);
            MDagPath dagPath;
            dagFn.getPath(dagPath);
            dagPath.extendToShape();
            SdfPath usdPrimPath;
            if (!TfMapLookup(mDagPathToUsdPathMap, dagPath, &usdPrimPath)) {
                continue;
            }
            usdPrimPath = usdPrimPath.ReplacePrefix(usdPrimPath.GetPrefixes()[0], usdVariantRootPrimPath); // Convert base to variant usdPrimPath
            tableOfActivePaths[usdPrimPath] = true;
            activePaths.push_back(usdPrimPath);
            //UsdPrim usdPrim = mStage->GetPrimAtPath(usdPrimPath);
            //usdPrim.SetActive(true);
        }
        if (!tableOfActivePaths.empty()) {
            { // == BEG: Scope for Variant EditContext
                // Create the variantSet and variant
                UsdVariantSet modelingVariantSet = usdVariantRootPrim.GetVariantSets().AddVariantSet("modelingVariant");
                modelingVariantSet.AddVariant(variantName);
                modelingVariantSet.SetVariantSelection(variantName);
                // Set the Edit Context
                UsdEditTarget editTarget = modelingVariantSet.GetVariantEditTarget();
                UsdEditContext editContext(mJobCtx.mStage, editTarget);

                // == Activate/Deactivate UsdPrims
                UsdPrimRange rng = UsdPrimRange::AllPrims(mJobCtx.mStage->GetPseudoRoot());
                std::vector<UsdPrim> primsToDeactivate;
                for (auto it = rng.begin(); it != rng.end(); ++it) {
                    UsdPrim usdPrim = *it;
                    // For all xformable usdPrims...
                    if (usdPrim && usdPrim.IsA<UsdGeomXformable>()) {
                        bool isActive=false;
                        for (const auto& activePath : activePaths) {
                            //primPathD.HasPrefix(primPathA);
                            if (usdPrim.GetPath().HasPrefix(activePath) ||
                                    activePath.HasPrefix(usdPrim.GetPath())) {
                                isActive=true; break;
                            }
                        }
                        if (isActive==false) {
                            primsToDeactivate.push_back(usdPrim);
                            it.PruneChildren();
                        }
                    }
                }
                // Now deactivate the prims (done outside of the UsdPrimRange
                // so not to modify the iterator while in the loop)
                for ( UsdPrim const& prim : primsToDeactivate ) {
                    prim.SetActive(false);
                }
            } // == END: Scope for Variant EditContext
        }
    } // END: RenderLayer iterations

    // Set the default modeling variant
    UsdVariantSet modelingVariantSet = usdVariantRootPrim.GetVariantSet("modelingVariant");
    if (modelingVariantSet.IsValid()) {
        modelingVariantSet.SetVariantSelection(defaultModelingVariant);
    }
    return defaultPrim;
}

bool usdWriteJob::needToTraverse(const MDagPath& curDag)
{
    return mJobCtx.needToTraverse(curDag);
}

void usdWriteJob::perFrameCallback(double iFrame)
{
    if (!mJobCtx.mArgs.melPerFrameCallback.empty()) {
        MGlobal::executeCommand(mJobCtx.mArgs.melPerFrameCallback.c_str(), true);
    }

    if (!mJobCtx.mArgs.pythonPerFrameCallback.empty()) {
        MGlobal::executePythonCommand(mJobCtx.mArgs.pythonPerFrameCallback.c_str(), true);
    }
}


// write the frame ranges and statistic string on the root
// Also call the post callbacks
void usdWriteJob::postCallback()
{
    if (!mJobCtx.mArgs.melPostCallback.empty()) {
        MGlobal::executeCommand(mJobCtx.mArgs.melPostCallback.c_str(), true);
    }

    if (!mJobCtx.mArgs.pythonPostCallback.empty()) {
        MGlobal::executePythonCommand(mJobCtx.mArgs.pythonPostCallback.c_str(), true);
    }
}



PXR_NAMESPACE_CLOSE_SCOPE

