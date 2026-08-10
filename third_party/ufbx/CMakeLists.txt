# The FBX reader of ADR 0035, built from upstream's two files unmodified.
add_library(homeworldz-ufbx STATIC ufbx.c)
target_include_directories(homeworldz-ufbx PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# ufbx is configured by macro, and every feature left on is more code reachable
# from a hostile file. Import needs geometry, materials, textures and skins;
# these four do other jobs and are switched off.
#
#   SUBDIVISION      unbounded work on a small file - a subdivided cage is a
#                    multiplier an uploader chooses
#   TESSELLATION     NURBS surfaces, which nothing in the corpus carries
#   GEOMETRY_CACHE   reads *further* external files (.mcx/.xml) named by the FBX,
#                    which is exactly the reach an archive import must not have
#   SCENE_EVALUATION,
#   SKINNING_EVALUATION,
#   ANIMATION_BAKING animation at a point in time; import takes the bind pose
#
# Triangulation and index generation stay on: FBX polygons are n-gons and the
# type-49 asset is indexed triangles.
target_compile_definitions(homeworldz-ufbx PUBLIC
  UFBX_NO_SUBDIVISION
  UFBX_NO_TESSELLATION
  UFBX_NO_GEOMETRY_CACHE
  UFBX_NO_SCENE_EVALUATION
  UFBX_NO_SKINNING_EVALUATION
  UFBX_NO_ANIMATION_BAKING
  # ufbx also reads OBJ. ADR 0035 names OBJ as the format after FBX, and this
  # define is where it turns on - left off until something actually imports it,
  # so an FBX import cannot be handed an OBJ by a caller that meant neither.
  UFBX_NO_FORMAT_OBJ)

# UFBX_NO_ERROR_STACK is deliberately *not* set. ADR 0035 makes import failure a
# reportable property of the asset rather than a lost upload, and "failed" is not
# a report - the error stack is what turns a refusal into something a creator can
# act on.

if(MSVC)
  # Upstream's own warning level, not the region's. This is vendored source we
  # do not modify, so warnings here are noise no one can act on without forking.
  target_compile_options(homeworldz-ufbx PRIVATE /W3)
else()
  target_compile_options(homeworldz-ufbx PRIVATE -w)
endif()
