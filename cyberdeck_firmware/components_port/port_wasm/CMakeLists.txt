# port_wasm_vm.c と port_wasm_natives.c は ESP-IDF に依存しない。
# ホストの ctest が同じソースをリンクして、実機と同じ経路で .wasm を回す
# （host/CMakeLists.txt を見よ）。IDF を使う口を増やすときは、
# port_wasm.c か port_wasm_backend.c の側に置くこと。
idf_component_register(SRCS "port_wasm_vm.c" "port_wasm_natives.c"
                            "port_wasm.c" "port_wasm_backend.c"
                       INCLUDE_DIRS "include"
                       REQUIRES cdk_wasm wasm3 port_periph port_storage
                                esp_timer esp_hw_support)
