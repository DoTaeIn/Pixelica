# Consumer project of cmake/PixelicaPackageConfigTest.cmake. One configure run per
# test case (-DCASE=<name>); a violated expectation is a FATAL_ERROR, so the
# configure step failing means the case failed. Cases that are expected to fail
# in find_package() simply call it with REQUIRED; the driver inverts the result.
#
# The stub export files of the fake prefixes append "<component>@<prefix id>" to
# the global property PIXELICA_STUB_LOADS every time one is included, and fail if the
# same target would be defined twice.
cmake_minimum_required(VERSION 3.21)
project(PackageConfigConsumer NONE)

if(NOT DEFINED CASE)
  message(FATAL_ERROR "CASE is required")
endif()

macro(expect_target t)
  if(NOT TARGET ${t})
    message(FATAL_ERROR "[${CASE}] expected target ${t}")
  endif()
endmacro()

macro(expect_no_target t)
  if(TARGET ${t})
    message(FATAL_ERROR "[${CASE}] target ${t} must not exist")
  endif()
endmacro()

macro(expect_true var)
  if(NOT ${var})
    message(FATAL_ERROR "[${CASE}] expected ${var} to be true (is '${${var}}')")
  endif()
endmacro()

macro(expect_false var)
  if(${var})
    message(FATAL_ERROR "[${CASE}] expected ${var} to be false (is '${${var}}')")
  endif()
endmacro()

function(expect_loads expected)
  get_property(loads GLOBAL PROPERTY PIXELICA_STUB_LOADS)
  if(NOT "${loads}" STREQUAL "${expected}")
    message(FATAL_ERROR "[${CASE}] export files loaded: '${loads}', expected '${expected}'")
  endif()
endfunction()

function(expect_links target dep)
  get_target_property(libs ${target} INTERFACE_LINK_LIBRARIES)
  if(NOT libs OR NOT dep IN_LIST libs)
    message(FATAL_ERROR "[${CASE}] ${target} must link ${dep} (INTERFACE_LINK_LIBRARIES='${libs}')")
  endif()
endfunction()

if(CASE STREQUAL "default-3d")
  # No components: the 3D engine when its export exists.
  find_package(Pixelica CONFIG REQUIRED)
  expect_target(Pixelica::core)
  expect_target(Pixelica::renderer3d)
  expect_target(Pixelica::common)
  expect_no_target(Pixelica::renderer2d)
  expect_true(Pixelica_core_FOUND)
  expect_links(Pixelica::core Pixelica::common)
  expect_loads("common@pixelica;core@pixelica")

elseif(CASE STREQUAL "default-2d")
  # No 3D export: the 2D renderer.
  find_package(Pixelica CONFIG REQUIRED)
  expect_target(Pixelica::renderer2d)
  expect_target(Pixelica::common)
  expect_target(Pixelica2D::core)
  expect_no_target(Pixelica::core)
  expect_links(Pixelica::renderer2d Pixelica::common)
  expect_loads("common@p2d;renderer2d@p2d")

elseif(CASE STREQUAL "default-common")
  find_package(Pixelica CONFIG REQUIRED)
  expect_target(Pixelica::common)
  expect_no_target(Pixelica::core)
  expect_no_target(Pixelica::renderer2d)
  expect_loads("common@pcommon")

elseif(CASE STREQUAL "default-empty")
  # The config exists but no export file does: REQUIRED must fail.
  find_package(Pixelica CONFIG REQUIRED)

elseif(CASE STREQUAL "explicit-2d-on-merged")
  # A merged prefix holds both engines; only what is requested is imported.
  find_package(Pixelica CONFIG REQUIRED COMPONENTS renderer2d)
  expect_target(Pixelica::renderer2d)
  expect_target(Pixelica::common)
  expect_no_target(Pixelica::core)
  expect_no_target(Pixelica::hybrid)
  expect_true(Pixelica_renderer2d_FOUND)
  expect_loads("common@pmerged;renderer2d@pmerged")

elseif(CASE STREQUAL "explicit-3d-on-merged")
  find_package(Pixelica CONFIG REQUIRED COMPONENTS renderer3d)
  expect_target(Pixelica::core)
  expect_target(Pixelica::renderer3d)
  expect_no_target(Pixelica::renderer2d)
  expect_true(Pixelica_renderer3d_FOUND)

elseif(CASE STREQUAL "hybrid-merged")
  find_package(Pixelica CONFIG REQUIRED COMPONENTS hybrid)
  foreach(t Pixelica::hybrid Pixelica::core Pixelica::renderer2d Pixelica::common)
    expect_target(${t})
  endforeach()
  expect_links(Pixelica::hybrid Pixelica::core)
  expect_links(Pixelica::hybrid Pixelica::renderer2d)
  expect_true(Pixelica_hybrid_FOUND)

elseif(CASE STREQUAL "hybrid-requires-both")
  # 3D SDK only: hybrid needs renderer2d too.
  find_package(Pixelica CONFIG REQUIRED COMPONENTS hybrid)

elseif(CASE STREQUAL "hybrid-optional")
  find_package(Pixelica CONFIG REQUIRED COMPONENTS core OPTIONAL_COMPONENTS hybrid renderer2d)
  expect_true(Pixelica_FOUND)
  expect_true(Pixelica_core_FOUND)
  expect_false(Pixelica_hybrid_FOUND)
  expect_false(Pixelica_renderer2d_FOUND)
  expect_no_target(Pixelica::hybrid)
  expect_target(Pixelica::core)

elseif(CASE STREQUAL "unknown-required")
  find_package(Pixelica CONFIG REQUIRED COMPONENTS core bogus)

elseif(CASE STREQUAL "unknown-optional")
  find_package(Pixelica CONFIG REQUIRED COMPONENTS core OPTIONAL_COMPONENTS bogus)
  expect_true(Pixelica_FOUND)
  expect_false(Pixelica_bogus_FOUND)

elseif(CASE STREQUAL "repeat")
  # Repeated and overlapping calls import each export file exactly once.
  find_package(Pixelica CONFIG REQUIRED COMPONENTS common)
  find_package(Pixelica CONFIG REQUIRED COMPONENTS core)
  find_package(Pixelica CONFIG REQUIRED COMPONENTS core common renderer3d)
  find_package(Pixelica CONFIG REQUIRED COMPONENTS renderer2d hybrid)
  find_package(Pixelica CONFIG REQUIRED)
  foreach(t Pixelica::common Pixelica::core Pixelica::renderer3d Pixelica::renderer2d Pixelica::hybrid)
    expect_target(${t})
  endforeach()
  expect_loads("common@pmerged;core@pmerged;renderer2d@pmerged;hybrid@pmerged")

elseif(CASE STREQUAL "rig-modules-need-common-only")
  find_package(Pixelica CONFIG REQUIRED COMPONENTS skeleton ik)
  expect_target(Pixelica::skeleton)
  expect_target(Pixelica::ik)
  expect_target(Pixelica::common)
  expect_no_target(Pixelica::core)
  expect_no_target(Pixelica::renderer2d)
  expect_links(Pixelica::skeleton Pixelica::common)
  expect_links(Pixelica::ik Pixelica::common)

elseif(CASE STREQUAL "pixelica2d-default")
  find_package(Pixelica2D CONFIG REQUIRED)
  expect_target(Pixelica2D::core)
  expect_target(Pixelica::renderer2d)
  expect_target(Pixelica::common)
  expect_no_target(Pixelica::core)
  expect_true(Pixelica2D_core_FOUND)
  expect_loads("common@p2d;renderer2d@p2d")
  # A later Pixelica lookup in the same project reuses the imported targets.
  find_package(Pixelica CONFIG REQUIRED COMPONENTS renderer2d common)
  expect_loads("common@p2d;renderer2d@p2d")

elseif(CASE STREQUAL "pixelica2d-core-common")
  find_package(Pixelica2D CONFIG REQUIRED COMPONENTS core common)
  expect_target(Pixelica2D::core)
  expect_target(Pixelica::common)
  expect_no_target(Pixelica::core)
  expect_true(Pixelica2D_core_FOUND)
  expect_true(Pixelica2D_common_FOUND)

elseif(CASE STREQUAL "pixelica2d-unknown-required")
  find_package(Pixelica2D CONFIG REQUIRED COMPONENTS core hybrid)

elseif(CASE STREQUAL "pixelica2d-needs-common-package")
  # Pixelica2DConfig.cmake without the Pixelica package files beside it.
  find_package(Pixelica2D CONFIG REQUIRED)

elseif(CASE STREQUAL "pixelica2d-ignores-other-pixelica")
  # An unrelated Pixelica install is also reachable (Pixelica_DIR points at it);
  # the 2D wrapper must still use the package beside itself.
  find_package(Pixelica2D CONFIG REQUIRED)
  expect_target(Pixelica2D::core)
  expect_loads("common@p2d;renderer2d@p2d")

else()
  message(FATAL_ERROR "unknown CASE '${CASE}'")
endif()

message(STATUS "[${CASE}] OK")
