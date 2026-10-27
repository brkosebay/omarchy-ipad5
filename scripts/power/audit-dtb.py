#!/usr/bin/env python3
"""Enumerate enabled j71t genpd consumers from an actual built DTB (offline)."""
import pathlib
import struct
import sys

blob = pathlib.Path(sys.argv[1]).read_bytes()
magic, _, off, strings, _, _, _, _, _, size = struct.unpack_from('>10I', blob)
if magic != 0xd00dfeed:
    raise SystemExit('not an FDT')
p, stack, nodes = off, [], {}
while p < off + size:
    token, = struct.unpack_from('>I', blob, p)
    p += 4
    if token == 1:
        end = blob.index(0, p)
        name = blob[p:end].decode()
        path = '/' + '/'.join(stack[1:] + [name]) if stack else '/'
        stack.append(name)
        nodes[path] = {}
        p = (end + 4) & ~3
    elif token == 2:
        stack.pop()
    elif token == 3:
        length, nameoff = struct.unpack_from('>II', blob, p)
        p += 8
        end = blob.index(0, strings + nameoff)
        name = blob[strings + nameoff:end].decode()
        nodes['/' + '/'.join(stack[1:])][name] = blob[p:p+length]
        p = (p + length + 3) & ~3
    elif token == 9:
        break
    elif token != 4:
        raise SystemExit(f'unknown token {token}')

def words(b):
    return struct.unpack('>' + 'I' * (len(b) // 4), b)

def string(b):
    return b.rstrip(b'\0').decode().replace('\0', ',')

phandles = {words(n['phandle'])[0]: (path, n) for path, n in nodes.items() if 'phandle' in n}
print('consumer\tcompatible\tdomains')
for path, node in nodes.items():
    if 'power-domains' not in node or '#power-domain-cells' in node:
        continue
    parts = path.split('/')[1:]
    ancestors = ['/' + '/'.join(parts[:i]) for i in range(len(parts) + 1)]
    if any(nodes.get(a, {}).get('status', b'okay\0') not in (b'okay\0', b'ok\0') for a in ancestors):
        continue
    cells, i, domains = words(node['power-domains']), 0, []
    while i < len(cells):
        domain, props = phandles[cells[i]]
        count = words(props.get('#power-domain-cells', b'\0'*4))[0]
        domains.append(string(props.get('label', domain.encode())))
        i += 1 + count
    print(path, string(node.get('compatible', b'')), ','.join(domains), sep='\t')
