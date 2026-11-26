import sys
from pathlib import Path


draw = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
context = Path(sys.argv[2]).read_text(encoding="utf-8-sig")
types = draw[draw.index("struct SetBinding {"):draw.index("static SRWLOCK descriptor_lock")]
scoped_images = "GameImage *original_images[SCOPE_SRVS];" in draw
if scoped_images:
    types = "#define SCOPED_GAME_IMAGES 1\n" + types
prefetch_start = draw.find("void VkScopePrefetchShaderResources(")
srv_start = draw.index("ID3D11ShaderResourceView* VkScopeShaderResource(")
prefetch = draw[prefetch_start:srv_start] if prefetch_start >= 0 else ""
srv_end = draw.index("\n\tif (view->kind != SPIRV_SSBO", srv_start)
pixel_srv = draw[srv_start:srv_end]
pixel_srv = pixel_srv.replace("\tPublishedConstants *published = NULL;\n", "")
pixel_srv = pixel_srv.replace("\tVkDeviceSize bytes = 0, buffer_bytes = 0;\n\tconst char *why;\n\tSetBuffer bound;\n", "")
pixel_srv += "\n\treturn NULL;\n}\n"
start = draw.find("static VkImageView game_texture_view(")
if start >= 0:
    memo_start = draw.index("struct ShownTexture {")
    memo_end = draw.index("// With the", memo_start)
    memo = draw[memo_start:memo_end]
    if scoped_images:
        memo += draw[draw.index("static ID3D11ShaderResourceView* shown_texture(GameImage *"):
                     draw.index("static ID3D11ShaderResourceView* shown_texture(VkImageView")]
    formats = "#define ORIGINAL_FORMATS 1\n" + memo + draw[start:prefetch_start]
else:
    start = draw.index("static ID3D11ShaderResourceView* game_texture(")
    end = prefetch_start if prefetch_start >= 0 else srv_start
    formats = draw[start:end]
lifetime = draw[draw.index("static void images_changing()"):draw.index("void VkModsImageCreated(")]
lifetime += draw[draw.index("void VkModsForgetImage("):draw.index("// With vk_dump_hashes, for the report: once for each image")]
bindings = context[context.index("void ShimContext::SetShaderResources("):context.index("void ShimContext::SetSamplers(")]
references = context[context.index("template <class Slot, class Object>"):context.index("ShimContext::ShimContext(")]
timed_start = draw.index("struct Timed {")
timed = draw[timed_start:draw.index("\n};", timed_start) + 3]
Path(sys.argv[3]).write_text(types, encoding="utf-8")
Path(sys.argv[4]).write_text(timed + "\n" + lifetime + formats + prefetch + pixel_srv + references + bindings, encoding="utf-8")
