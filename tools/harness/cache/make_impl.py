import sys
from pathlib import Path


source = Path(sys.argv[1]).read_text(encoding="utf-8-sig")
start = source.index("void RegionHashesCache::Initialize(")
end = source.index("// Helper function that clears region hash cache", start)
Path(sys.argv[2]).write_text(source[start:end], encoding="utf-8")
