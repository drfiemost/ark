set(RUNTIME_OUTPUT_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})

kde4_add_unit_test(clirartest NOGUI clirartest.cpp ../cliplugin.cpp)
target_link_libraries(clirartest kerfuffle Qt4::QtTest ${KDE4_KPARTS_LIBRARY})
