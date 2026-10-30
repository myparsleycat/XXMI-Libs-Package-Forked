import sys
from pathlib import Path


draw = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
context = Path(sys.argv[2]).read_text(encoding="utf-8-sig")
types = draw[draw.index("struct SetBinding {"):draw.index("static SRWLOCK descriptor_lock")]
start = draw.find("void VkScopePrefetchShaderResources(")
prefetch = draw[start:draw.index("ID3D11ShaderResourceView* VkScopeShaderResource(", start)] if start >= 0 else ""
getter = context[context.index("void ShimContext::GetShaderResources("):context.index("void ShimContext::SetSamplers(")]
timed_start = draw.index("struct Timed {")
timed = draw[timed_start:draw.index("\n};", timed_start) + 3]
Path(sys.argv[3]).write_text(types, encoding="utf-8")
Path(sys.argv[4]).write_text(timed + "\n" + prefetch + getter, encoding="utf-8")
