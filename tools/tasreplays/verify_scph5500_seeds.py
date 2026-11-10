"""Verify the target BIOS seed selection against owned ROMs; emit no ROM bytes."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/'tools'))
import filter_bios_seeds  # noqa: E402
REFERENCE_SHA='71af94d1e47a68c11e8fdb9f8368040601514a42a5a399cda48c7d3bff1e99d3'
TARGET_SHA='9c0421858e217805f4abe18698afea8d5aa36ff0727eb8484944e00eb5e7eadb'


def derive(seeds, reference, target):
    kept=[];dropped=[]
    for seed in seeds:
        offset=int(seed['address'],16)-0xBFC00000
        (kept if 0<=offset and offset+64<=len(reference) and
            reference[offset:offset+64]==target[offset:offset+64] else dropped).append(seed)
    return kept,dropped


def verify(reference_path,target_path):
    reference,target=[p.read_bytes() for p in [reference_path,target_path]]
    if any(len(b)!=0x80000 for b in [reference,target]):raise ValueError('512 KiB BIOS required')
    if hashlib.sha256(reference).hexdigest()!=REFERENCE_SHA or hashlib.sha256(target).hexdigest()!=TARGET_SHA:
        raise ValueError('wrong owned BIOS identity')
    seed_path=ROOT/'recompiler/seeds/phase2_ghidra_seeds_SCPH5500.json'
    text,kept,dropped,added=filter_bios_seeds.derive_text(
        str(ROOT/'recompiler/seeds/phase2_ghidra_seeds.json'),reference,target,'SCPH5500',
        str(ROOT/'bios/SCPH5500.toml'),str(target_path))
    if seed_path.read_text(encoding='utf-8')!=text:
        raise ValueError('target seeds differ from tools/filter_bios_seeds.py derivation')
    if not any(int(s['address'],16)==0xBFC00000 for s in kept):raise ValueError('reset vector missing')
    if any(int(s['address'],16)>=0xBFC18000 for s in kept):raise ValueError('shell native admission is not qualified')
    if reference[0x10000:0x18000]!=target[0x10000:0x18000]:raise ValueError('claimed Kernel Part 2 bytes differ')
    return {'matched_window_seeds':len(kept)-added,'dropped_window_seeds':len(dropped),'kernel_table_added':added,
            'total_admitted_seeds':len(kept),'reference_sha256':REFERENCE_SHA,'target_sha256':TARGET_SHA,
            'seed_sha256':hashlib.sha256(seed_path.read_bytes()).hexdigest(),
            'scope':'64-byte windows, architectural vectors and SCPH5500 kernel table targets; shell interpreted; whole native validation pending'}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--reference',type=Path,required=True);p.add_argument('--target',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args()
    result=verify(a.reference,a.target)
    with a.output.open('x') as stream:json.dump(result,stream,indent=2)
    print(json.dumps(result))
