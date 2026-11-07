"""Build the re-vendored ScarletNX renderer (runtime/snxvk/src_new, ScarletNX main 3f15d931fb4b + our hooks) into
runtime/snxvk/libsnxvk_new.a (SWR_NO_DLSS=1: libsnxvk_new_nodlss.a). Same scheme as build_snxvk.py: every global it
defines is renamed snxv_<name>, so its GPU front end sits next to ours; snxvk_glue.c provides what it needs from the
runtime. Also builds and runs upstream's shader decoder and settings generators (tools/) first, as its cmake does."""
import os, subprocess, sys, shutil, concurrent.futures as cf
HERE = os.path.dirname(os.path.abspath(__file__)); ROOT = os.path.dirname(HERE)
V = os.path.join(ROOT, 'runtime', 'snxvk'); S = os.path.join(V, 'src_new'); NX = os.path.join(S, 'nx')
G = os.environ.get('SWR_GCC_BIN') or os.path.join(ROOT, 'gcc', 'Library', 'bin')   # SWR_GCC_BIN: set by build.bat
NODLSS = bool(os.environ.get('SWR_NO_DLSS'))
OUT = os.path.join(V, 'obj_new_nodlss' if NODLSS else 'obj_new'); os.makedirs(OUT, exist_ok=True)
LIB = os.path.join(V, 'libsnxvk_new_nodlss.a' if NODLSS else 'libsnxvk_new.a')
# upstream's code generators (GPL-3.0, same project), vendored next to the sources
TOOLS = os.path.join(S, 'tools')
# the versions and SHA-256 hashes ScarletNX's cmake/dependencies.cmake pins
DEPS = {k: os.path.join(V, 'deps', d) for k, d in (
    ('volk', 'volk-vulkan-sdk-1.4.363.0'), ('vma', 'VulkanMemoryAllocator-3.4.0'), ('vkh', 'Vulkan-Headers-1.4.367'),
    ('xxh', 'xxHash-0.8.4'), ('zstd', 'zstd-1.5.7'))}
# upstream src/nx/CMakeLists.txt: the gpu, shader and vk components, less what our runtime does itself or doesn't
# have: the NVN driver path (gpu/nvn, off by default upstream: nvn_direct), FFmpeg NVDEC (vk/nvdec.c; our host1x
# decodes video) and the SDL window (common/gpu/vk/window.c). vk/uma.c is built but kept off (vk_memory=1, copied);
# runtime/snxvk_glue.c answers for its platform hooks. gpu/vk/swr_glue.c is the renderer interface as plain functions
SRC = ('gpu/gpu.c gpu/maxwell/engines.c gpu/maxwell/mme.c gpu/maxwell/compute.c gpu/maxwell/fermi2d.c gpu/maxwell/dma.c '
       'gpu/maxwell/write_pages.c gpu/draw_state.c gpu/maxwell/draw_state.c gpu/maxwell/maxwell3d.c gpu/renderer.c '
       'gpu/shipped_pass.c '
       'gpu/shader/shader.c gpu/shader/decode.c gpu/shader/corpus.c gpu/shader/shipped.c gpu/shader/spirv_translate.c '
       'gpu/shader/spirv_compute.c gpu/shader/structure.c gpu/shader/shared_reach.c gpu/shader/registers.c '
       'gpu/shader/forms.c gpu/shader/dataflow.c gpu/shader/emit_int.c gpu/shader/emit_float.c gpu/shader/emit_memory.c '
       'gpu/shader/emit_texture.c gpu/shader/emit_io.c gpu/shader/emit_flow.c gpu/shader/emit_thread.c '
       'gpu/shader/advanced_blend_spirv.c '
       'gpu/vk/renderer.c gpu/vk/device.c gpu/vk/descriptors.c gpu/vk/cmd.c gpu/vk/record.c gpu/vk/format.c '
       'gpu/vk/surface.c gpu/vk/texture.c gpu/vk/buffer.c gpu/vk/writeback.c gpu/vk/pipeline.c gpu/vk/pipeline_file.c '
       'memory/own_win.c '
       'gpu/vk/render.c gpu/vk/release.c gpu/vk/query.c gpu/vk/xfb.c gpu/vk/display.c gpu/vk/compute.c '
       'gpu/vk/pending_pages.c gpu/vk/image_out.c gpu/vk/depth_pack.c gpu/vk/memory.c gpu/vk/renderdoc.c gpu/vk/astc.c '
       'gpu/vk/blocklinear.c gpu/vk/texconv.c gpu/vk/owned.c gpu/vk/dma.c gpu/vk/blit.c gpu/vk/msaa.c gpu/vk/ui.c '
       'gpu/texture/blocklinear.c gpu/texture/depth.c gpu/vk/vic.c gpu/vk/gpuprof.c gpu/vk/ftz_probe.c '
       'gpu/vk/vertex_input.c gpu/vk/draw_texture.c gpu/vk/tess_passthrough.c gpu/vk/xxh3.c gpu/vk/uma.c '
       'gpu/vk/hdr.c gpu/vk/swr_settings.c gpu/vk/swr_glue.c').split() + (['gpu/vk/dlss_stub.c'] if NODLSS else ['gpu/vk/dlss.c', 'gpu/vk/fsr_fg.c'])
COMMON = ['common/gpu/vk/ring.c', 'common/gpu/vk/swapchain.c', 'common/gpu/vk/vk_common.c', 'common/utilities/spirv_emit.c']
REDIRECT = {'g_opt_vsync': 'snxvk_opt_vsync'}
GEN = os.path.join(OUT, 'shaderdec')
INC = ['-I' + S, '-I' + NX, '-I' + GEN, '-I' + DEPS['volk'], '-I' + os.path.join(DEPS['vma'], 'include'),
       '-I' + os.path.join(DEPS['vkh'], 'include'), '-I' + DEPS['xxh'], '-I' + os.path.join(DEPS['zstd'], 'lib'),
       '-I' + os.path.join(ROOT, 'runtime'),
       '-I' + os.path.join(V, 'deps', 'FidelityFX-SDK-2.4.0')]  # AMD's MIT API headers, for fsr_fg.c
# SNX_SHIP_WALK: upstream's cmake/ship_walk.cmake hashes shipped_pass.c into it, a key of its shader files
CF = ['-std=gnu2x', '-O2', '-fno-strict-aliasing', '-DVK_NO_PROTOTYPES', '-DVK_USE_PLATFORM_WIN32_KHR',
      '-DSNX_SHIP_WALK="swr-3f15d931"', '-w'] + INC
ZSTD_CF = ['-std=gnu11', '-O2', '-w', '-DZSTD_DISABLE_ASM=1', '-DZSTD_MULTITHREAD_SUPPORT_DEFAULT=0',
           '-I' + os.path.join(DEPS['zstd'], 'lib'), '-I' + os.path.join(DEPS['zstd'], 'lib', 'common')]
ENV = dict(os.environ, PATH=G + os.pathsep + os.environ.get('PATH', ''))


def cc(args):
    r = subprocess.run(args, capture_output=True, text=True, env=ENV)
    if r.returncode:
        sys.exit('build failed: ' + ' '.join(args[-3:]) + '\n' + r.stderr[-4000:])


def shaderdec():
    os.makedirs(GEN, exist_ok=True)
    exe = os.path.join(OUT, 'shaderdec.exe')
    cc([os.path.join(G, 'gcc.exe'), '-std=c99', '-O2', os.path.join(TOOLS, 'shaderdec', 'shaderdec.c'), '-o', exe])
    cc([exe, '--enum', os.path.join(GEN, 'maxwell_opcodes.h'), '--decoder', os.path.join(GEN, 'maxwell_decode.inc'),
        os.path.join(NX, 'gpu', 'shader', 'maxwell_opcodes.def')])


def settingsgen():
    exe = os.path.join(OUT, 'settingsgen.exe')
    cc([os.path.join(G, 'gcc.exe'), '-std=c99', '-O2', os.path.join(TOOLS, 'settingsgen', 'settingsgen.c'), '-o', exe])
    cc([exe, '--out', GEN, os.path.join(NX, 'runtime', 'settings.txt')])


# glslang 16.6.0 (the version ScarletNX's cmake pins): GLSLANG=path, or glslang on PATH, or third_party\glslang\bin
GLSLANG = (os.environ.get('GLSLANG') or shutil.which('glslang')
           or os.path.join(ROOT, 'third_party', 'glslang', 'bin', 'glslang.exe'))
# upstream src/nx/CMakeLists.txt snx_glsl_spirv: source, array name, header, defines
SPV = [('gpu/texture/astc.comp', 'astc_spv', 'nx/gpu/texture/astc_spv.h', []),
       ('gpu/texture/bc7.comp', 'bc7_spv', 'nx/gpu/texture/bc7_spv.h', []),
       ('gpu/texture/blocklinear.comp', 'blocklinear_spv', 'nx/gpu/texture/blocklinear_spv.h', []),
       ('gpu/texture/texconv.comp', 'texconv_spv', 'nx/gpu/texture/texconv_spv.h', []),
       ('gpu/video/vic.comp', 'vic_spv', 'nx/gpu/video/vic_spv.h', []),
       ('gpu/video/vic.comp', 'vic_unaligned_spv', 'nx/gpu/video/vic_spv.h', ['VIC_UNALIGNED']),
       ('gpu/vk/query_predicate.comp', 'query_predicate_spv', 'nx/gpu/vk/query_predicate_spv.h', []),
       ('gpu/vk/query_count.comp', 'query_count_spv', 'nx/gpu/vk/query_count_spv.h', []),
       ('gpu/vk/xfb_indirect.comp', 'xfb_indirect_spv', 'nx/gpu/vk/xfb_indirect_spv.h', []),
       ('gpu/vk/depth_pack.comp', 'depth_pack_spv', 'nx/gpu/vk/depth_pack_spv.h', []),
       ('gpu/vk/query_compare.comp', 'query_compare_spv', 'nx/gpu/vk/query_compare_spv.h', []),
       ('gpu/vk/ftz_probe.comp', 'ftz_probe_spv', 'nx/gpu/vk/ftz_probe_spv.h', []),
       ('gpu/vk/msaa.vert', 'msaa_vert_spv', 'nx/gpu/vk/msaa_spv.h', []),
       ('gpu/vk/msaa.frag', 'msaa_unpack_spv', 'nx/gpu/vk/msaa_spv.h', ['MS_UNPACK']),
       ('gpu/vk/msaa.frag', 'msaa_unpack_depth_spv', 'nx/gpu/vk/msaa_spv.h', ['MS_UNPACK', 'MS_DEPTH']),
       ('gpu/vk/msaa.frag', 'msaa_pack_spv', 'nx/gpu/vk/msaa_spv.h', ['MS_PACK'])]


def spirv():   # cmake/glsl.cmake + spirv_source.cmake: the SPIR-V words in a C array beside its header
    out = []
    for src, name, header, defs in SPV:
        words = os.path.join(GEN, name + '.inc')
        cc([GLSLANG, '-V', '--target-env', 'vulkan1.1', '-g0', '-x'] + ['-D' + d for d in defs] + ['-o', words, os.path.join(NX, src)])
        c = os.path.join(GEN, name + '.c')
        with open(c, 'w') as f:
            f.write('/* generated from %s by glslang (build_snxvk2.py) */\n#include "%s"\n' % (src, header))
            f.write('const u32 %s[] = {\n#include "%s.inc"\n};\nconst u64 %s_size = sizeof %s;\n' % (name, name, name, name))
        out.append(c)
    return out


def obj(path):
    rel = os.path.relpath(path, V).replace(os.sep, '_').replace('/', '_')
    return os.path.join(OUT, rel + '.o')


def main():
    shaderdec()
    settingsgen()
    spv_c = spirv()
    gcc = os.path.join(G, 'gcc.exe')
    srcs = [os.path.join(NX, f) for f in SRC] + [os.path.join(S, f) for f in COMMON] + spv_c + \
           [os.path.join(GEN, 'settings.c'), os.path.join(DEPS['volk'], 'volk.c')]   # settings.c: g_settings, defaults
    jobs = [(obj(c), [gcc] + CF + ['-c', c, '-o', obj(c)]) for c in srcs]
    zdir = os.path.join(DEPS['zstd'], 'lib')
    for sub in ('common', 'compress', 'decompress'):   # upstream cmake/dependencies.cmake snx_zstd
        for f in sorted(os.listdir(os.path.join(zdir, sub))):
            if f.endswith('.c'):
                c = os.path.join(zdir, sub, f)
                jobs.append((obj(c), [gcc] + ZSTD_CF + ['-c', c, '-o', obj(c)]))
    def compile_one(j):
        r = subprocess.run(j[1], capture_output=True, text=True, env=ENV)
        return j[1][-3], r.returncode, r.stderr
    with cf.ThreadPoolExecutor(os.cpu_count()) as ex: done = list(ex.map(compile_one, jobs))
    failed = [d for d in done if d[1]]
    if failed:   # every file's errors at once, not only the first one's
        sys.exit('build failed:\n' + '\n'.join(src + '\n' + err[-3000:] for src, _, err in failed))
    vma = os.path.join(OUT, 'vma.o')
    cc([os.path.join(G, 'g++.exe'), '-std=c++17', '-O2', '-w', '-DVK_NO_PROTOTYPES'] + INC + ['-c', os.path.join(NX, 'gpu/vk/vma.cpp'), '-o', vma])
    merged = os.path.join(OUT, 'snxvk_c.o')
    cc([os.path.join(G, 'ld.exe'), '-r', '-o', merged] + [o for o, _ in jobs])
    nm = subprocess.run([os.path.join(G, 'nm.exe'), '--defined-only', '-g', merged, vma], capture_output=True, text=True).stdout.split('\n')
    defs = sorted({l.split()[-1] for l in nm if len(l.split()) == 3 and not l.split()[-1].startswith('_Z')})
    with open(os.path.join(OUT, 'rename.txt'), 'w') as f:
        for n in defs: f.write(f'{n} snxv_{n}\n')
        for a, b in REDIRECT.items(): f.write(f'{a} {b}\n')
    renamed = os.path.join(OUT, 'snxvk_renamed.o')
    cc([os.path.join(G, 'objcopy.exe'), '--redefine-syms=' + os.path.join(OUT, 'rename.txt'), merged, renamed])
    if os.path.exists(LIB): os.remove(LIB)
    vma_r = os.path.join(OUT, 'vma_renamed.o')
    cc([os.path.join(G, 'objcopy.exe'), '--redefine-syms=' + os.path.join(OUT, 'rename.txt'), vma, vma_r])
    cc([os.path.join(G, 'ar.exe'), 'rcs', LIB, renamed, vma_r])
    und = []
    for o in (renamed, vma_r):
        out = subprocess.run([os.path.join(G, 'nm.exe'), '-u', o], capture_output=True, text=True).stdout
        und += [l.split()[-1] for l in out.splitlines() if l.split()]
    have = {'snxv_' + n for n in defs}
    ext = sorted(x for x in set(und) if x not in have and not x.startswith('__imp_') and not x.startswith('___'))
    open(os.path.join(OUT, 'externals.txt'), 'w').write('\n'.join(ext) + '\n')
    print(f'{LIB}: {len(defs)} symbols renamed; {len(ext)} external references (externals.txt)')


main()
