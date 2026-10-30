import sys
from pathlib import Path


source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
definitions = source[source.index("const UINT SHADOW_PAGE ="):source.index("static void finish_refresh_of(")]
fill = source[source.index("void VkScopeFillShadow("):source.index("// The bones of everything the game draws")]
start = source.index("ConstantsSource *source = &constants_sources[&object->r];")
end = source.index("object->r.version++;", start) + len("object->r.version++;")
reset = "static void republish(ShimBuffer *object)\n{\n" + source[start:end] + "\n}\n"
reset = reset.replace(";\n", ";\n\tUINT bytes = std::get<2>(source->key), stride = std::get<3>(source->key);\n", 1)
objects = Path(sys.argv[2]).read_text(encoding="utf-8-sig")
upload = objects[objects.index("UINT64 Dx12ShimBufferAddress("):].rstrip()
arguments = "buffer, 1, offset, size" if "UINT64 offset, UINT64 size)" in upload else "buffer, 1"
adapter = "static UINT64 range_address(ShimBuffer *buffer, UINT64 offset, UINT64 size)\n{\n"
adapter += "return Dx12ShimBufferAddress(" + arguments + ")" + (" + offset" if arguments == "buffer, 1" else "") + ";\n}\n"
Path(sys.argv[3]).write_text(definitions + fill + reset + upload + "\n" + adapter, encoding="utf-8")
