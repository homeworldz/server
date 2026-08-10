add_library(homeworldz-region-core
  src/appearance_bake.cpp
  src/avatar_controller.cpp
  src/api_models.cpp
  src/falcon_runtime.cpp
  src/grid_client.cpp
  src/bake.cpp
  src/http_response.cpp
  src/image.cpp
  src/inventory_asset.cpp
  src/object_asset.cpp
  src/parcel.cpp
  src/physics_scene.cpp
  src/physics_scenarios.cpp
  src/region_config.cpp
  src/region_extensions.cpp
  src/region_transit.cpp
  src/capability_paths.cpp
  src/region_storage.cpp
  src/scene.cpp
  src/sha256.cpp
  src/simulation_loop.cpp
  src/visual_params.cpp
  src/wearable.cpp)
target_sources(homeworldz-region-core PRIVATE src/terrain_edit.cpp)
target_sources(homeworldz-region-core PRIVATE src/viewer_protocol.cpp)
target_sources(homeworldz-region-core PRIVATE src/viewer_capabilities.cpp)
target_sources(homeworldz-region-core PRIVATE src/session_protocol.cpp)
target_sources(homeworldz-region-core PRIVATE src/session_server.cpp)
target_sources(homeworldz-region-core PRIVATE src/mesh_acceptance.cpp)
target_sources(homeworldz-region-core PRIVATE src/avatar_joints.cpp)
target_sources(homeworldz-region-core PRIVATE src/slmesh.cpp)
target_sources(homeworldz-region-core PRIVATE src/mesh_convert.cpp)
target_sources(homeworldz-region-core PRIVATE src/gltf_from_slmesh.cpp)
target_sources(homeworldz-region-core PRIVATE src/rig_check.cpp)
target_sources(homeworldz-region-core PRIVATE src/llsd_xml.cpp)
target_sources(homeworldz-region-core PRIVATE src/llsd_binary.cpp)
target_sources(homeworldz-region-core PRIVATE src/render_material.cpp)
target_sources(homeworldz-region-core PRIVATE src/texture_entry.cpp)
target_sources(homeworldz-region-core PRIVATE src/mesh_model_upload.cpp)
target_sources(homeworldz-region-core PRIVATE src/mesh_publish.cpp)
# FBX import (ADR 0035). ufbx is vendored rather than packaged, so this adds a
# translation unit and no dependency the cloud box has to be prepared for.
target_sources(homeworldz-region-core PRIVATE src/fbx_import.cpp)
target_link_libraries(homeworldz-region-core PRIVATE homeworldz-ufbx)
find_package(meshoptimizer CONFIG REQUIRED)
target_link_libraries(homeworldz-region-core PRIVATE meshoptimizer::meshoptimizer)
find_path(HOMEWORLDZ_CGLTF_INCLUDE_DIR cgltf.h REQUIRED)
target_include_directories(homeworldz-region-core PRIVATE ${HOMEWORLDZ_CGLTF_INCLUDE_DIR})
# stb_image decodes the PNG and JPEG that glTF embeds; the region already had
# JPEG2000 and TGA and neither of the two formats a GLB actually carries
# (ADR 0033 M3).
find_path(HOMEWORLDZ_STB_INCLUDE_DIR stb_image.h REQUIRED)
target_include_directories(homeworldz-region-core PRIVATE ${HOMEWORLDZ_STB_INCLUDE_DIR})
find_package(ZLIB REQUIRED)
target_link_libraries(homeworldz-region-core PRIVATE ZLIB::ZLIB)
find_package(libwebsockets CONFIG REQUIRED)
# vcpkg's dynamic triplets export websockets_shared; static ones export
# websockets. Take whichever this build provides.
if(TARGET websockets_shared)
  target_link_libraries(homeworldz-region-core PUBLIC websockets_shared)
else()
  target_link_libraries(homeworldz-region-core PUBLIC websockets)
endif()
target_include_directories(homeworldz-region-core PUBLIC include)
target_compile_features(homeworldz-region-core PUBLIC cxx_std_20)
target_compile_definitions(homeworldz-region-core PUBLIC
  HOMEWORLDZ_VERSION="${HOMEWORLDZ_VERSION}")
target_link_libraries(homeworldz-region-core PUBLIC homeworldz-sqlite homeworldz-script homeworldz-openjpeg)
if(HOMEWORLDZ_JOLT_AVAILABLE)
  target_sources(homeworldz-region-core PRIVATE src/jolt_world.cpp)
  target_link_libraries(homeworldz-region-core PUBLIC Jolt::Jolt)
  target_compile_definitions(homeworldz-region-core PUBLIC HOMEWORLDZ_JOLT_AVAILABLE=1)
endif()
if(HOMEWORLDZ_PHYSX_AVAILABLE)
  target_sources(homeworldz-region-core PRIVATE src/physx_world.cpp)
  target_link_libraries(homeworldz-region-core PUBLIC unofficial::omniverse-physx-sdk::sdk)
endif()

add_executable(homeworldz-region src/main.cpp)
target_link_libraries(homeworldz-region PRIVATE homeworldz-region-core)

if(MSVC)
  target_compile_options(homeworldz-region PRIVATE /W4 /permissive-)
  target_link_libraries(homeworldz-region-core PUBLIC ws2_32)
else()
  target_compile_options(homeworldz-region PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(homeworldz-region-tests tests/http_response_test.cpp)
target_link_libraries(homeworldz-region-tests PRIVATE homeworldz-region-core)
target_compile_definitions(homeworldz-region-tests PRIVATE
  HOMEWORLDZ_OPERATIONAL_CONTRACT_PATH="${CMAKE_SOURCE_DIR}/api/contracts/operational.tsv")
add_test(NAME region-http-response COMMAND homeworldz-region-tests)

add_executable(homeworldz-image-tests tests/image_test.cpp)
target_link_libraries(homeworldz-image-tests PRIVATE homeworldz-region-core)
add_test(NAME region-image COMMAND homeworldz-image-tests)

add_executable(homeworldz-wearable-tests tests/wearable_test.cpp)
target_link_libraries(homeworldz-wearable-tests PRIVATE homeworldz-region-core)
add_test(NAME region-wearable COMMAND homeworldz-wearable-tests)

add_executable(homeworldz-bake-tests tests/bake_test.cpp)
target_link_libraries(homeworldz-bake-tests PRIVATE homeworldz-region-core)
add_test(NAME region-bake COMMAND homeworldz-bake-tests)

add_executable(homeworldz-appearance-bake-tests tests/appearance_bake_test.cpp)
target_link_libraries(homeworldz-appearance-bake-tests PRIVATE homeworldz-region-core)
add_test(NAME region-appearance-bake COMMAND homeworldz-appearance-bake-tests)

add_executable(homeworldz-visual-params-tests tests/visual_params_test.cpp)
target_link_libraries(homeworldz-visual-params-tests PRIVATE homeworldz-region-core)
add_test(NAME region-visual-params COMMAND homeworldz-visual-params-tests)

add_executable(homeworldz-bake-diag tests/bake_diag.cpp)
target_link_libraries(homeworldz-bake-diag PRIVATE homeworldz-region-core)

add_executable(homeworldz-inventory-asset-tests tests/inventory_asset_test.cpp)
target_link_libraries(homeworldz-inventory-asset-tests PRIVATE homeworldz-region-core)
add_test(NAME region-inventory-asset COMMAND homeworldz-inventory-asset-tests)

add_executable(homeworldz-scene-tests tests/scene_test.cpp)
target_link_libraries(homeworldz-scene-tests PRIVATE homeworldz-region-core)
add_test(NAME region-scene COMMAND homeworldz-scene-tests)

add_executable(homeworldz-session-protocol-tests tests/session_protocol_test.cpp)
target_link_libraries(homeworldz-session-protocol-tests PRIVATE homeworldz-region-core)
add_executable(homeworldz-mesh-acceptance-tests tests/mesh_acceptance_test.cpp)
target_link_libraries(homeworldz-mesh-acceptance-tests PRIVATE homeworldz-region-core)
add_test(NAME region-mesh-acceptance COMMAND homeworldz-mesh-acceptance-tests)
add_executable(homeworldz-mesh-diag src/mesh_diag_main.cpp)
target_link_libraries(homeworldz-mesh-diag PRIVATE homeworldz-region-core)
# The FBX side of the same question (ADR 0035). Not a test: the corpus it reads
# is local-only for licence reasons, so it has nothing to assert against.
add_executable(homeworldz-fbx-diag src/fbx_diag_main.cpp)
target_link_libraries(homeworldz-fbx-diag PRIVATE homeworldz-region-core homeworldz-ufbx)
target_include_directories(homeworldz-fbx-diag PRIVATE src)
add_executable(homeworldz-fbx-import-tests tests/fbx_import_test.cpp)
target_link_libraries(homeworldz-fbx-import-tests PRIVATE homeworldz-region-core)
add_test(NAME region-fbx-import COMMAND homeworldz-fbx-import-tests)
add_executable(homeworldz-avatar-joints-tests tests/avatar_joints_test.cpp)
target_link_libraries(homeworldz-avatar-joints-tests PRIVATE homeworldz-region-core)
# Every test executable in this file needs an add_test beside it. Three did not
# (slmesh, mesh-convert, mesh-acceptance): compiled on every build and never run,
# so the suite reported "37/37 passed" while the mesh gate's own tests sat
# unexecuted - one of them failing since rigged acceptance was turned on
# 2026-08-08 and reported green three times. Building a test is not running it,
# and a binary nobody invokes fails silently for as long as nobody looks.
add_executable(homeworldz-slmesh-tests tests/slmesh_test.cpp)
target_link_libraries(homeworldz-slmesh-tests PRIVATE homeworldz-region-core)
add_test(NAME region-slmesh COMMAND homeworldz-slmesh-tests)
add_executable(homeworldz-mesh-convert-tests tests/mesh_convert_test.cpp)
target_link_libraries(homeworldz-mesh-convert-tests PRIVATE homeworldz-region-core)
add_test(NAME region-mesh-convert COMMAND homeworldz-mesh-convert-tests)
add_executable(homeworldz-gltf-from-slmesh-tests tests/gltf_from_slmesh_test.cpp)
target_link_libraries(homeworldz-gltf-from-slmesh-tests PRIVATE homeworldz-region-core)
add_executable(homeworldz-rig-check-tests tests/rig_check_test.cpp)
target_link_libraries(homeworldz-rig-check-tests PRIVATE homeworldz-region-core)
add_test(NAME region-rig-check COMMAND homeworldz-rig-check-tests)
add_test(NAME region-avatar-joints COMMAND homeworldz-avatar-joints-tests)
add_test(NAME region-gltf-from-slmesh COMMAND homeworldz-gltf-from-slmesh-tests)
add_executable(homeworldz-llsd-xml-tests tests/llsd_xml_test.cpp)
target_link_libraries(homeworldz-llsd-xml-tests PRIVATE homeworldz-region-core)
add_test(NAME region-llsd-xml COMMAND homeworldz-llsd-xml-tests)
add_executable(homeworldz-mesh-model-upload-tests tests/mesh_model_upload_test.cpp)
target_link_libraries(homeworldz-mesh-model-upload-tests PRIVATE homeworldz-region-core)
add_test(NAME region-mesh-model-upload COMMAND homeworldz-mesh-model-upload-tests)
add_executable(homeworldz-terrain-layerdata-tests tests/terrain_layerdata_test.cpp)
target_link_libraries(homeworldz-terrain-layerdata-tests PRIVATE homeworldz-region-core)
add_test(NAME region-terrain-layerdata COMMAND homeworldz-terrain-layerdata-tests)
add_executable(homeworldz-llsd-binary-tests tests/llsd_binary_test.cpp)
target_link_libraries(homeworldz-llsd-binary-tests PRIVATE homeworldz-region-core)
add_test(NAME region-llsd-binary COMMAND homeworldz-llsd-binary-tests)
add_executable(homeworldz-seed-capabilities-tests tests/seed_capabilities_test.cpp)
target_link_libraries(homeworldz-seed-capabilities-tests PRIVATE homeworldz-region-core)
add_test(NAME region-seed-capabilities COMMAND homeworldz-seed-capabilities-tests)
add_executable(homeworldz-texture-entry-tests tests/texture_entry_test.cpp)
target_link_libraries(homeworldz-texture-entry-tests PRIVATE homeworldz-region-core)
add_test(NAME region-texture-entry COMMAND homeworldz-texture-entry-tests)
add_executable(homeworldz-render-material-tests tests/render_material_test.cpp)
target_link_libraries(homeworldz-render-material-tests PRIVATE homeworldz-region-core)
add_test(NAME region-render-material COMMAND homeworldz-render-material-tests)
add_executable(homeworldz-capability-paths-tests tests/capability_paths_test.cpp)
target_link_libraries(homeworldz-capability-paths-tests PRIVATE homeworldz-region-core)
add_test(NAME region-capability-paths COMMAND homeworldz-capability-paths-tests)
add_executable(homeworldz-meshsmith src/meshsmith_main.cpp)
target_link_libraries(homeworldz-meshsmith PRIVATE homeworldz-region-core)
add_test(NAME region-session-protocol COMMAND homeworldz-session-protocol-tests)

add_executable(homeworldz-grid-client-tests tests/grid_client_test.cpp)
target_link_libraries(homeworldz-grid-client-tests PRIVATE homeworldz-region-core)
add_test(NAME region-grid-client COMMAND homeworldz-grid-client-tests)

add_executable(homeworldz-region-transit-tests tests/region_transit_test.cpp)
target_link_libraries(homeworldz-region-transit-tests PRIVATE homeworldz-region-core)
add_test(NAME region-transit COMMAND homeworldz-region-transit-tests)

add_executable(homeworldz-object-asset-tests tests/object_asset_test.cpp)
target_link_libraries(homeworldz-object-asset-tests PRIVATE homeworldz-region-core)
add_test(NAME region-object-asset COMMAND homeworldz-object-asset-tests)

add_executable(homeworldz-parcel-tests tests/parcel_test.cpp)
target_link_libraries(homeworldz-parcel-tests PRIVATE homeworldz-region-core)
add_test(NAME region-parcel COMMAND homeworldz-parcel-tests)

add_executable(homeworldz-region-config-tests tests/region_config_test.cpp)
target_link_libraries(homeworldz-region-config-tests PRIVATE homeworldz-region-core)
add_test(NAME region-config COMMAND homeworldz-region-config-tests)

add_executable(homeworldz-region-extension-tests tests/region_extensions_test.cpp)
target_link_libraries(homeworldz-region-extension-tests PRIVATE homeworldz-region-core)
add_test(NAME region-extensions COMMAND homeworldz-region-extension-tests)

add_executable(homeworldz-storage-tests tests/region_storage_test.cpp)
target_link_libraries(homeworldz-storage-tests PRIVATE homeworldz-region-core)
add_test(NAME region-storage COMMAND homeworldz-storage-tests)

add_executable(homeworldz-physics-contract-tests tests/physics_contract_test.cpp)
target_link_libraries(homeworldz-physics-contract-tests PRIVATE homeworldz-region-core)
add_test(NAME region-physics-contract COMMAND homeworldz-physics-contract-tests)

add_executable(homeworldz-physics-scene-tests tests/physics_scene_test.cpp)
target_link_libraries(homeworldz-physics-scene-tests PRIVATE homeworldz-region-core)
add_test(NAME region-physics-scene COMMAND homeworldz-physics-scene-tests)

add_executable(homeworldz-viewer-protocol-tests tests/viewer_protocol_test.cpp)
target_link_libraries(homeworldz-viewer-protocol-tests PRIVATE homeworldz-region-core)
add_test(NAME region-viewer-protocol COMMAND homeworldz-viewer-protocol-tests)

add_executable(homeworldz-avatar-controller-tests tests/avatar_controller_test.cpp)
target_link_libraries(homeworldz-avatar-controller-tests PRIVATE homeworldz-region-core)
add_test(NAME region-avatar-controller COMMAND homeworldz-avatar-controller-tests)

add_executable(homeworldz-terrain-edit-tests tests/terrain_edit_test.cpp)
target_link_libraries(homeworldz-terrain-edit-tests PRIVATE homeworldz-region-core)
add_test(NAME region-terrain-edit COMMAND homeworldz-terrain-edit-tests)

add_executable(homeworldz-script-runtime-tests tests/script_runtime_test.cpp)
target_link_libraries(homeworldz-script-runtime-tests PRIVATE homeworldz-region-core)
add_test(NAME region-script-runtime COMMAND homeworldz-script-runtime-tests)

add_executable(homeworldz-falcon-runtime-tests tests/falcon_runtime_test.cpp)
target_link_libraries(homeworldz-falcon-runtime-tests PRIVATE homeworldz-region-core)
add_test(NAME region-falcon-runtime COMMAND homeworldz-falcon-runtime-tests)

if(HOMEWORLDZ_JOLT_AVAILABLE AND HOMEWORLDZ_PHYSX_AVAILABLE)
  add_executable(homeworldz-physics-adapter-smoke tests/physics_adapters_test.cpp)
  target_link_libraries(homeworldz-physics-adapter-smoke PRIVATE homeworldz-region-core)
  add_test(NAME region-physics-adapters COMMAND homeworldz-physics-adapter-smoke)
  add_executable(homeworldz-physics-scenario-tests tests/physics_scenarios_test.cpp)
  target_link_libraries(homeworldz-physics-scenario-tests PRIVATE homeworldz-region-core)
  add_test(NAME region-physics-scenarios COMMAND homeworldz-physics-scenario-tests)
  add_executable(homeworldz-physics-lab src/physics_lab.cpp)
  target_link_libraries(homeworldz-physics-lab PRIVATE homeworldz-region-core)
  if(WIN32)
    target_link_libraries(homeworldz-physics-lab PRIVATE psapi)
  endif()
endif()

