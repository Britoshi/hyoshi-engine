# Desktop only: it writes files that are then committed (see src/Main.cpp).
add_executable(hyoshi_asset_cooker src/Main.cpp)
set_target_properties(hyoshi_asset_cooker PROPERTIES OUTPUT_NAME hyoshi-asset-cooker)
target_link_libraries(hyoshi_asset_cooker PRIVATE hyoshi::renderer hyoshi_stb)
hyoshi_set_warnings(hyoshi_asset_cooker)
hyoshi_configure_executable(hyoshi_asset_cooker)
