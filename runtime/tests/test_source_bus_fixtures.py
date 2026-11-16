"""Authored optional source bus integration at O0/O2 (GNU link-fixture helper)."""
import argparse
from pathlib import Path
import tempfile
from fixture_link import build_and_run

FIXTURES = {
    'cd-clock': (['dma.c','dma_gpu_ll.c','gpu_timeline.c','psx_cycles.c','psx_sha256.c'],
                 'test_source_bus_cd_clock.c',('PSX_ENABLE_BLOCK_CYCLES=1',),
                 ('-Wno-unused-function',)),
    'runtime': (['source_gpu_runtime.c','dma.c','dma_gpu_ll.c','mdec.c','spu.c',
                 'gpu_sw_renderer.c','gpu_render.c','gpu_vram_dirty.c','crc32.c',
                 'psx_video_timing.c','gpu_timeline.c','audio_trace.c'], 'test_source_bus_runtime.c',
                ('PSX_SOURCE_BUS_BOUNDARY=1','PSX_ENABLE_BLOCK_CYCLES=1'),
                ('-Wno-unused-function','-Wno-unused-variable','-Wno-misleading-indentation','-Wno-comment')),
    'service': (['source_gpu_runtime.c'],'test_source_gpu_service_path.c',('PSX_SOURCE_BUS_BOUNDARY=1',),()),
    'raster': (['gpu_sw_renderer.c','gpu_vram_dirty.c'],'test_source_gpu_raster_rows.c',(),()),
    'receipt': (['dma.c','dma_gpu_ll.c','gpu_timeline.c'],'source_bus_receipt.c',(),()),
}

FIXTURES['prefix']=(FIXTURES['runtime'][0],FIXTURES['runtime'][1],FIXTURES['runtime'][2]+('PSX_TEST_SOURCE_PREFIX=1',),FIXTURES['runtime'][3])
FIXTURES['frozen']=(FIXTURES['runtime'][0],FIXTURES['runtime'][1],FIXTURES['runtime'][2]+('PSX_TEST_FROZEN_SOURCE=1',),FIXTURES['runtime'][3])

def run_case(cc, here, opt, work, name,extra_defines=()):
    modules, source, defines, warnings = FIXTURES[name]
    return build_and_run(cc,here,here.parent,opt,work,modules,source,
                         defines=defines+tuple(extra_defines),fixture_warnings=warnings)

if __name__ == '__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc',default='gcc')
    parser.add_argument('--case',choices=FIXTURES,required=True)
    args=parser.parse_args()
    here=Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory() as root:
        for opt in ('-O0','-O2'):
            print(run_case(args.cc,here,opt,Path(root),args.case),flush=True)
    print('source bus '+args.case+': O0/O2 PASS')
