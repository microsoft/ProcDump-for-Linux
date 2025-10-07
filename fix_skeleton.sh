#!/bin/bash
#
# Post-process the generated eBPF skeleton header to add explicit void* cast
#
if [ -f "procdump_ebpf.skel.h" ]; then
    sed -i 's/s->data = procdump_ebpf__elf_bytes(&s->data_sz);/s->data = (void *)procdump_ebpf__elf_bytes(\&s->data_sz);/g' "procdump_ebpf.skel.h"
    echo "Applied void* cast to skeleton file"
else
    echo "Warning: procdump_ebpf.skel.h not found"
fi
