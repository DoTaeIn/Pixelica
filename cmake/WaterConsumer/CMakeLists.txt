cmake_minimum_required(VERSION 3.21)
project(WaterConsumer LANGUAGES C)

# A real external consumer: it knows nothing but the install prefix. Called
# twice to prove a repeat find_package() imports nothing twice.
find_package(PixelicaWater 0.1 CONFIG REQUIRED)
find_package(PixelicaWater 0.1 CONFIG REQUIRED)
if(NOT TARGET Pixelica::water)
  message(FATAL_ERROR "PixelicaWater did not provide Pixelica::water")
endif()
# The core must not drag in a renderer or an unrelated optional module.
if(NOT PIXELICA_WATER_EXPECT_3D)
  foreach(unexpected Pixelica::core Pixelica::renderer2d Pixelica::water3d
                     Pixelica::particles Pixelica::shaders Pixelica::audio)
    if(TARGET ${unexpected})
      message(FATAL_ERROR "the water core package imported ${unexpected}")
    endif()
  endforeach()
endif()

add_executable(water_consumer main.c)
target_link_libraries(water_consumer PRIVATE Pixelica::water)
enable_testing()
add_test(NAME water_consumer COMMAND water_consumer)

# The adapter is a SEPARATE component; ask for it only where it was installed.
if(PIXELICA_WATER_EXPECT_3D)
  find_package(PixelicaWater 0.1 CONFIG REQUIRED COMPONENTS water3d)
  if(NOT TARGET Pixelica::water3d)
    message(FATAL_ERROR "component water3d was found but Pixelica::water3d does not exist")
  endif()
  add_executable(water3d_consumer main3d.c)
  target_link_libraries(water3d_consumer PRIVATE Pixelica::water3d)
  add_test(NAME water3d_consumer COMMAND water3d_consumer)
endif()
