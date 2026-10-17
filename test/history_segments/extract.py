# Pull the segment scheduler functions verbatim out of UITask.cpp.
import re, sys
src = open(sys.argv[1]).read()
names = ["segGatherRange", "segCountRange", "segBuildJob", "segCommitJob",
         "segMoreWorkPending", "segMarkSeqDirty", "segNoteEvicted"]
out = []
for n in names:
    m = re.search(r'^static [^\n;]*\b' + n + r'\([^;{]*\)\s*\{', src, re.M | re.S)
    if not m:
        if n == "segCountRange": continue   # absent in pre-fix sources
        sys.exit("missing " + n)
    i = src.index('{', m.end() - 1); depth = 0; j = i
    while True:
        c = src[j]
        if c == '{': depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0: break
        j += 1
    out.append(src[m.start():j + 1])
print("\n\n".join(out))
