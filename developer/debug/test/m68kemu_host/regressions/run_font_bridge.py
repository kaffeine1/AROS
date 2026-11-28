#!/usr/bin/env python3
"""Compile and run the font bridge host regression tests (TextAttr/TTextAttr and TextFont names)."""
# Copyright (C) 2026, The AROS Development Team. All rights reserved.

import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess  # nosec B404 - developer host test must invoke compiler
import tempfile


def extract_function(source, name, filename):
    masked = re.sub(
        r'//[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'',
        lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]),
        source,
    )
    match = re.search(
        r'(?m)^[^\s;{}][^\n;{}]*\b' + re.escape(name) +
        r'\s*\([^;{}]*\)[^;{}]*\{',
        masked,
    )
    if not match:
        raise ValueError(f'definition of {name} not found in {filename}')
    end = masked.index('{', match.start()) + 1
    depth = 1
    while depth:
        depth += (masked[end] == '{') - (masked[end] == '}')
        end += 1
    line = source[:match.start()].count('\n') + 1
    return f'#line {line} "{filename}"\n' + source[match.start():end] + '\n'


def extract_regex(source, pattern, desc, filename):
    match = re.search(pattern, source)
    if not match:
        raise ValueError(f'{desc} not found in {filename}')
    line = source[:match.start()].count('\n') + 1
    return f'#line {line} "{filename}"\n' + match.group(0) + '\n'


def run(args, output):
    root = args.source_root
    mem_src = (root / 'rom/m68kemu/m68kemu_memory.c').read_text(encoding='latin-1')
    shadow_src = (root / 'rom/m68kemu/m68kemu_shadow.c').read_text(encoding='latin-1')
    layouts_src = (root / 'rom/m68kemu/m68kemu_shadow_layouts.h').read_text(encoding='latin-1')
    thunks_src = (root / 'rom/m68kemu/m68kemu_thunks.c').read_text(encoding='latin-1')
    gen_src = (root / 'rom/m68kemu/m68kemu_thunks_gen.c').read_text(encoding='latin-1')
    intern_src = (root / 'rom/m68kemu/m68kemu_intern.h').read_text(encoding='latin-1')
    offsets_src = (root / 'rom/m68kemu/m68kemu_offsets.h').read_text(encoding='latin-1')

    def need_define(text, name):
        m = re.search(r'^\s*#define\s+' + re.escape(name) + r'\s+([^\n/]+)', text, re.M)
        if not m:
            raise ValueError(f'{name} not found')
        return f'#define {name} {m.group(1).strip()}\n'

    consts = [
        need_define(intern_src, 'M68KEMU_MAX_SHADOWS'),
        need_define(offsets_src, 'M68KEMU_HEAP_HDR'),
        need_define(offsets_src, 'M68KEMU_HEAP_MIN_SPLIT'),
        need_define(intern_src, 'M68KEMU_CHIP_END'),
        need_define(intern_src, 'M68KEMU_ROM_BASE'),
        need_define(intern_src, 'M68KEMU_ROM_SIZE'),
        need_define(intern_src, 'M68KEMU_ROM_END'),
        need_define(intern_src, 'RGN_CHIP'),
        need_define(intern_src, 'RGN_FAST'),
        need_define(intern_src, 'FB_NEXT'),
        need_define(intern_src, 'FB_SIZE'),
        need_define(offsets_src, 'M68K_TA_NAME'),
        need_define(offsets_src, 'M68K_TA_YSIZE'),
        need_define(offsets_src, 'M68K_TA_STYLE'),
        need_define(offsets_src, 'M68K_TA_FLAGS'),
        need_define(shadow_src, 'SHADOW_GLYPHS'),
        need_define(thunks_src, 'SHADOW_OWNRP'),
    ]
    if re.search(r'^\s*#define\s+SHADOW_FONTNAME\b', shadow_src, re.M):
        consts.append(need_define(shadow_src, 'SHADOW_FONTNAME'))

    shadow_field_macros = extract_regex(
        layouts_src,
        r'(?ms)^#define SHADOW_FIELD_TYPE\(.*?^#define SHADOW_FIELD\(.*?0, 0, NULL \}',
        'SHADOW_FIELD macros',
        'm68kemu_shadow_layouts.h',
    )
    fields_textfont = extract_regex(
        layouts_src,
        r'(?ms)^static const struct M68KFieldMap shadow_fields_TextFont\[\] = \{.*?\};',
        'shadow_fields_TextFont',
        'm68kemu_shadow_layouts.h',
    )
    row_textfont = extract_regex(
        layouts_src,
        r'(?m)^\s*\{\s*"TextFont",\s*\d+,\s*\d+,\s*shadow_fields_TextFont\s*\},',
        'TextFont layout row',
        'm68kemu_shadow_layouts.h',
    )

    extracted = [
        extract_function(intern_src, 'm68k_in_mem', 'm68kemu_intern.h'),
        extract_function(mem_src, 'heap_publish', 'm68kemu_memory.c'),
        extract_function(mem_src, 'M68KEmu_HeapRegion', 'm68kemu_memory.c'),
        extract_function(mem_src, 'heap_largest', 'm68kemu_memory.c'),
        extract_function(mem_src, 'heap_alloc_in', 'm68kemu_memory.c'),
        extract_function(mem_src, 'M68KEmu_HeapAlloc', 'm68kemu_memory.c'),
        extract_function(mem_src, 'M68KEmu_HeapFree', 'm68kemu_memory.c'),
        shadow_field_macros,
        fields_textfont,
        'static const struct M68KStructLayout shadow_gen_layouts[] = {\n',
        row_textfont,
        '    { NULL, 0, 0, NULL }\n};\n',
        extract_function(thunks_src, 'shadow_register', 'm68kemu_thunks.c'),
        extract_function(thunks_src, 'shadow_lookup', 'm68kemu_thunks.c'),
        extract_function(thunks_src, 'shadow_remove', 'm68kemu_thunks.c'),
        extract_function(shadow_src, 'sync_fields', 'm68kemu_shadow.c'),
        extract_function(shadow_src, 'shadow_find_layout', 'm68kemu_shadow.c'),
    ]
    if re.search(r'^ULONG m68k_font_name\s*\(', shadow_src, re.M):
        extracted.append(extract_function(shadow_src, 'm68k_font_name', 'm68kemu_shadow.c'))
    extracted += [
        extract_function(shadow_src, 'm68k_textfont_extras', 'm68kemu_shadow.c'),
        extract_function(shadow_src, 'shadow_create', 'm68kemu_shadow.c'),
        extract_function(shadow_src, 'shadow_destroy', 'm68kemu_shadow.c'),
        extract_function(shadow_src, 'shadow_create_by_name', 'm68kemu_shadow.c'),
        extract_function(thunks_src, 'textattr_in', 'm68kemu_thunks.c'),
    ]
    if re.search(r'^static BOOL ttextattr_in\s*\(', thunks_src, re.M):
        extracted.append(extract_function(thunks_src, 'ttextattr_in', 'm68kemu_thunks.c'))
    extracted += [
        extract_function(thunks_src, 'thunk_gfx_AskFont', 'm68kemu_thunks.c'),
        extract_function(thunks_src, 'thunk_gfx_OpenFont', 'm68kemu_thunks.c'),
        extract_function(thunks_src, 'thunk_diskfont_OpenDiskFont_m', 'm68kemu_thunks.c'),
        extract_function(gen_src, 'thunk_graphics_CloseFont', 'm68kemu_thunks_gen.c'),
    ]

    output.mkdir(parents=True, exist_ok=True)
    (output / 'font_consts.inc').write_text(''.join(consts), encoding='latin-1')
    (output / 'font_extracted.inc').write_text('\n'.join(extracted), encoding='latin-1')
    if args.prepare_only:
        return

    command = shlex.split(args.cxx) + [
        '-std=c++17', '-Wall', '-Wextra', '-Werror',
        '-Wno-unused-function', '-Wno-unused-parameter',
        '-g', '-O1', '-I' + str(output),
        str(Path(__file__).with_name('test_font_bridge.cpp')),
        '-o', str(output / 'test_font_bridge'),
    ]
    if args.sanitize:
        command += ['-fsanitize=address,undefined', '-fno-sanitize-recover=all']
    subprocess.run(command, check=True)  # nosec B603

    env = os.environ.copy()
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=0')
    subprocess.run([str(output / 'test_font_bridge')], check=True, env=env)  # nosec B603


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path,
                        default=Path(__file__).resolve().parents[5])
    parser.add_argument('--build-dir', type=Path)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'c++'))
    parser.add_argument('--sanitize', action='store_true')
    parser.add_argument('--prepare-only', action='store_true',
                        help='write the extracted sources and stop: no compiler, no run')
    args = parser.parse_args()
    if args.build_dir:
        run(args, args.build_dir.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix='m68kemu-font-bridge-') as path:
            run(args, Path(path))


if __name__ == '__main__':
    main()
