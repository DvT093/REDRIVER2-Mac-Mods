# Extract a folder from a PS1 CD image (raw 2352-byte sectors).
# Far city needs: cd game && for d in LEVELS NLEVELS; do python3 ../src/mac/extract_levels.py install/Driver2CD1.bin mod/DRIVER2/$d DRIVER2/$d; done
import sys, struct, os
img, outdir, want = sys.argv[1], sys.argv[2], sys.argv[3]
f = open(img, 'rb')
SS = 2352
def sector(n):
    f.seek(n * SS); raw = f.read(SS)
    return raw[24:24+2048]  # mode2 form1 user data
def read_extent(lba, size):
    out = bytearray(); n = 0
    while len(out) < size:
        out += sector(lba + n); n += 1
    return bytes(out[:size])
pvd = sector(16)
assert pvd[1:6] == b'CD001', pvd[:8]
root = pvd[156:156+34]
def entries(lba, size):
    data = read_extent(lba, size); i = 0
    while i < len(data):
        l = data[i]
        if l == 0:
            i = (i // 2048 + 1) * 2048; continue
        rec = data[i:i+l]
        elba = struct.unpack('<I', rec[2:6])[0]; esize = struct.unpack('<I', rec[10:14])[0]
        flags = rec[25]; nl = rec[32]; name = rec[33:33+nl].decode('latin1')
        yield name.split(';')[0], elba, esize, flags & 2
        i += l
rlba = struct.unpack('<I', root[2:6])[0]; rsize = struct.unpack('<I', root[10:14])[0]
def find(lba, size, path):
    for name, elba, esize, isdir in entries(lba, size):
        if name.upper() == path[0].upper():
            if len(path) == 1: return elba, esize, isdir
            return find(elba, esize, path[1:])
    return None
# list top-level
print([e[0] for e in entries(rlba, rsize) if e[0] not in ('\x00', '\x01')])
d = find(rlba, rsize, want.split('/'))
if not d: print('not found'); sys.exit(1)
elba, esize, isdir = d
os.makedirs(outdir, exist_ok=True)
for name, flba, fsize, sub in entries(elba, esize):
    if name in ('\x00', '\x01') or sub: continue
    path = os.path.join(outdir, name)
    with open(path, 'wb') as o: o.write(read_extent(flba, fsize))
    print('extracted', name, fsize)
