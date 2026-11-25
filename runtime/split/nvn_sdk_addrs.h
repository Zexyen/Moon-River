/* runtime/split: nvn_sdk_addrs.h for the runtime built apart from the player's tables (README.md). The sdk offsets of
 * runtime/gen/nvn_sdk_addrs.h are data in game.so (runtime/mr_tables.c), read at run time. */
#ifndef NVN_SDK_ADDRS_H
#define NVN_SDK_ADDRS_H
extern const unsigned long long mr_sdka_chip_descriptor;
extern const unsigned long long mr_sdka_format_count;
extern const unsigned long long mr_sdka_format_table;
extern const unsigned long long mr_sdka_nvnTextureBuilderGetRawStorageClass;
extern const unsigned long long mr_sdka_nvnTextureBuilderGetSparseTileLayout;
extern const unsigned long long mr_sdka_nvnTextureBuilderGetStorageAlignment;
extern const unsigned long long mr_sdka_nvnTextureBuilderGetStorageClass;
extern const unsigned long long mr_sdka_nvnTextureBuilderGetStorageSize;
extern const unsigned long long mr_sdka_nvnTextureGetRawStorageClass;
extern const unsigned long long mr_sdka_nvnTextureGetSparseTileLayout;
extern const unsigned long long mr_sdka_nvnTextureGetStorageClass;
extern const unsigned long long mr_sdka_nvnTextureGetStorageSize;
extern const unsigned long long mr_sdka_texture_layout;
extern const unsigned long long mr_sdka_vt_hal_gm200;
extern const unsigned long long mr_sdka_vt_nvgpu_device;
#define SDKA_chip_descriptor                      mr_sdka_chip_descriptor
#define SDKA_format_count                         mr_sdka_format_count
#define SDKA_format_table                         mr_sdka_format_table
#define SDKA_nvnTextureBuilderGetRawStorageClass  mr_sdka_nvnTextureBuilderGetRawStorageClass
#define SDKA_nvnTextureBuilderGetSparseTileLayout mr_sdka_nvnTextureBuilderGetSparseTileLayout
#define SDKA_nvnTextureBuilderGetStorageAlignment mr_sdka_nvnTextureBuilderGetStorageAlignment
#define SDKA_nvnTextureBuilderGetStorageClass     mr_sdka_nvnTextureBuilderGetStorageClass
#define SDKA_nvnTextureBuilderGetStorageSize      mr_sdka_nvnTextureBuilderGetStorageSize
#define SDKA_nvnTextureGetRawStorageClass         mr_sdka_nvnTextureGetRawStorageClass
#define SDKA_nvnTextureGetSparseTileLayout        mr_sdka_nvnTextureGetSparseTileLayout
#define SDKA_nvnTextureGetStorageClass            mr_sdka_nvnTextureGetStorageClass
#define SDKA_nvnTextureGetStorageSize             mr_sdka_nvnTextureGetStorageSize
#define SDKA_texture_layout                       mr_sdka_texture_layout
#define SDKA_vt_hal_gm200                         mr_sdka_vt_hal_gm200
#define SDKA_vt_nvgpu_device                      mr_sdka_vt_nvgpu_device
#endif
