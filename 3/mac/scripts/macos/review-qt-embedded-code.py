#!/usr/bin/env python3
"""Review fixed Qt 6.11.0 payload using installed SBOM/features, GN and symbols.

This is a notice-coverage review of the selected payload, not a claim that the
original Homebrew compile commands or linker map have been recovered.
"""
from __future__ import annotations
import argparse
from collections import defaultdict
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import struct
import tarfile

ROOT=Path(__file__).resolve().parents[2]
WORK=ROOT/'build/mac-embedded-review'
INPUTS=ROOT/'build/mac-license-check'
SPEC=importlib.util.spec_from_file_location('notice_generator',ROOT/'scripts/macos/generate-license-bundle.py')
GEN=importlib.util.module_from_spec(SPEC);SPEC.loader.exec_module(GEN)
PINNED={'qtbase':'231ad85979864d914dc9568a1b71c91d6cf20d7b2021d059103bf0eb51cb755e',
 'qtsvg':'dfa8d653be07087d9407ed4a4ebae847f8953e0b7abd829f089803ab652a30e6', 'qtimageformats':'d3adb02ac5e2fe24068dbdaee0d7cc68cc3fa8553291c1bfce77c9fe8e940cc8',
 'qtwebengine':'63b921c8b2dd59152ced9a796676010166df044588ee00ef9429dc2fd2146736'}
CHROMIUM='src/3rdparty/chromium/'
PDFIUM=CHROMIUM+'third_party/pdfium/'
PA=CHROMIUM+'base/allocator/partition_allocator/'


def sha(path):
 h=hashlib.sha256()
 with path.open('rb') as stream:
  while data:=stream.read(1024*1024):h.update(data)
 return h.hexdigest()


def write(path,data):
 path.parent.mkdir(parents=True,exist_ok=True);path.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n')


def extract(component):
 entry=component['downloads'][0];archive=INPUTS/entry['file'];name=component['id'];destination=WORK/'sources'/name
 assert component['version']=='6.11.0',(name,component['version'])
 assert entry['sha256']==PINNED[name] and sha(archive)==PINNED[name],('Unreviewed Qt source',name)
 index=WORK/(name+'-source-index.json')
 if index.exists():
  data=json.loads(index.read_text())
  if data['archiveSha256']==entry['sha256'] and all((destination/p).is_file() and sha(destination/p)==h for p,h in data['files'].items()):return destination,data
 files={};destination.mkdir(parents=True,exist_ok=True)
 code_scopes=[PDFIUM,PA,CHROMIUM+'base/third_party/',*[CHROMIUM+'third_party/'+p+'/' for p in ['freetype','libjpeg_turbo','fast_float','abseil-cpp']]]
 with tarfile.open(archive) as tar:
  for member in tar:
   relative=Path(*Path(member.name).parts[1:]);text=str(relative)
   if not member.isfile() or '..' in relative.parts or member.size>8*1024*1024:continue
   selected=member.name.endswith(('.gn','.gni','CMakeLists.txt','.cmake','qt_attribution.json'))
   if name!='qtwebengine':selected=selected or text.startswith('src/3rdparty/') or text in ['src/gui/painting/qbrush.cpp','src/gui/painting/webgradients.cpp','src/dbus/dbus_minimal_p.h']
   else:selected=selected or (any(text.startswith(s) for s in code_scopes) and relative.suffix in ['.c','.cc','.cpp','.h','.hpp','.mm','.S','.inc','.txt','.json']) or (relative.name in ['LICENSE','COPYING','README.chromium'] and text.startswith(CHROMIUM))
   if selected:
    target=destination/relative;target.parent.mkdir(parents=True,exist_ok=True);target.write_bytes(tar.extractfile(member).read());files[text]=sha(target)
 data={'archive':entry['file'],'archiveSha256':entry['sha256'],'files':files};write(index,data)
 print('Pinned audit source:',name,len(files),'files',flush=True)
 return destination,data


def parse_sbom(text):
 packages={};files={};links=defaultdict(set);contains=defaultdict(set)
 for block in re.split(r'(?=^(?:PackageName|FileName): )',text,flags=re.M):
  identifier=re.search(r'^SPDXID: (.*)$',block,re.M)
  if not identifier:continue
  if block.startswith('PackageName: '):packages[identifier[1]]=block
  elif block.startswith('FileName: '):files[identifier[1]]=block
 for a,relation,b in re.findall(r'^Relationship: (\S+) (DEPENDS_ON|CONTAINS) (\S+)',text,re.M):
  # SPDX identifiers already include the module name and are globally unique here.
  b=b.split(':',1)[1] if b.startswith('DocumentRef-') else b
  (links if relation=='DEPENDS_ON' else contains)[a].add(b)
 return packages,files,links,contains


def leading_header(text):
 lines=[];block=False
 for line in text.splitlines():
  clean=line.strip()
  if block:lines.append(line);block='*/' not in line;continue
  if not clean:
   if lines:lines.append(line)
   continue
  if clean.startswith('//'):lines.append(line);continue
  if clean.startswith('/*'):lines.append(line);block='*/' not in line;continue
  break
 value='\n'.join(lines).strip()
 return value if re.search('copyright|permission|license|redistribution',value,re.I) else ''


def main():
 parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--app',type=Path,default=ROOT/'build/mac-bundle-check/VTR-PON.app');args=parser.parse_args()
 WORK.mkdir(parents=True,exist_ok=True);app=args.app.resolve();lock=json.loads((INPUTS/'inputs.json').read_text());bundle=json.loads((ROOT/'build/mac-bundle-check/bundle-manifest.json').read_text())
 components={c['id']:c for c in lock['components']};roots={};indices={};evidence={};receipt_builds={}
 for name in PINNED:
  component=components[name]
  assert sha(INPUTS/component['recipe'])==component['recipeSha256']
  assert sha(INPUTS/component['receipt'])==component['receiptSha256']
  receipt_builds[name]=json.loads((INPUTS/component['receipt']).read_text()).get('built_on')
  roots[name],indices[name]=extract(component)
 def source(module,name):
  path=roots[module]/name;assert path.is_file(),name
  evidence[module+'/'+name]={'kind':'pinned-source','sha256':sha(path),'sourceArchiveSha256':indices[module]['archiveSha256']};return path.read_text(errors='replace')
 packages={};files={};links=defaultdict(set);contains=defaultdict(set)
 for module,sbom in [('qtbase','qtbase'),('qtsvg','qtsvg'),('qtimageformats','qtimageformats'),('qtwebengine','qtpdf')]:
  cellar=Path(components[module]['cellar']);path=cellar/'share/qt/sbom'/(sbom+'-6.11.0.spdx');text=path.read_text();evidence[str(path)]={'kind':'installed-sbom','sha256':sha(path)}
  p,f,l,c=parse_sbom(text);packages.update(p);files.update(f)
  for a,b in l.items():links[a].update(b)
  for a,b in c.items():contains[a].update(b)
 qt_files=[f for f in lock['files'] if f['component'].startswith('qt')];bindings=[];root_ids=set();symbols={}
 origins={f['bundledPath']:f for f in bundle['originalFiles']};recorded_binaries={f['path']:f['sha256'] for f in bundle['files']}
 for entry in qt_files:
  relative=entry['path'];binary=app/relative;origin=origins[relative]
  assert sha(Path(origin['source']))==origin['sha256'],relative
  assert sha(binary)==recorded_binaries[relative],('App differs from provenance manifest',relative)
  assert struct.unpack_from('<I',binary.read_bytes(),4)[0]==0x0100000c,('Expected arm64',relative)
  # Stage copies are already tied to their original package by inputs.json.
  candidates=[fid for fid,block in files.items() if Path(block.splitlines()[0].removeprefix('FileName: ')).name==binary.name]
  assert len(candidates)==1,(relative,candidates)
  parents={pid for pid,children in contains.items() if candidates[0] in children and pid in packages};assert len(parents)==1,(relative,parents)
  root_ids.update(parents)
  p=subprocess.run(['/usr/bin/nm','-an',str(binary)],capture_output=True,text=True);assert p.returncode==0,p.stderr
  (WORK/(binary.name+'-symbols.txt')).write_text(p.stdout);symbols[binary.name]=p.stdout
  bindings.append({'path':relative,'contentSha256':GEN.macho_content_sha(binary),'originalSha256':origin['sha256'],'sbomTarget':next(iter(parents)),
   'compiledSourcePaths':re.findall(r'^RelationshipComment: (/[^\n]+)$',files[candidates[0]],re.M)})
 features={}
 for module in PINNED:
  cellar=Path(components[module]['cellar'])
  for path in cellar.glob('lib/*.framework/Versions/A/Headers/**/*config*.h'):
   text=path.read_text(errors='replace');values={k:int(v) for k,v in re.findall(r'^#define QT_FEATURE_(\w+) (-?1)$',text,re.M)}
   if values:features.update(values);evidence[str(path)]={'kind':'installed-feature-header','sha256':sha(path)}
 assert all(features.get('pdf_'+f)==-1 for f in ['v8','xfa','xfa_bmp','xfa_gif','xfa_png','xfa_tiff'])
 queue=list(root_ids);visited=set()
 while queue:
  at=queue.pop()
  if at in visited:continue
  visited.add(at);queue.extend(links[at])
 raw=json.loads((INPUTS/'notice-inputs.json').read_text());attributes=[];required=[]
 gui=source('qtbase','src/gui/CMakeLists.txt');assert 'qt_internal_extend_target(Gui CONDITION WIN32' in gui and 'rhi/cs_mipmap_p.h' in gui
 for pid in sorted(visited):
  block=packages.get(pid,'');idmatch=re.search(r'^    Id: (.+)$',block,re.M)
  if not idmatch:continue
  attr_id=idmatch[1];module='qtsvg' if '-qtsvg-' in pid else 'qtbase';match=re.search(r'/src_dir/'+module+r'/([^\n]*qt_attribution\.json)',block);assert match,(pid,'No source attribution')
  member=match[1].strip();text=source(module,member);values=json.loads(text,strict=False);values=values if isinstance(values,list) else [values];value=next(v for v in values if v['Id']==attr_id)
  decision='include';reason='Attribution dependency of a shipped Qt target; retain headers and generated data as well as compiled code.'
  if attr_id in ['extra-cmake-modules','kwin']:decision='build-only';reason='CMake build helpers of interface target Platform; source archive retains originals; no runtime library code claimed.'
  if attr_id=='rhi-miniengine-d3d12-mipmap':decision='excluded-platform';reason='SBOM attribution is unconditionally declared, but Gui CMake sources are under WIN32; selected payload is macOS arm64.'
  record=next(r for r in raw if r['component']==module and str(Path(r['path']).relative_to(Path('notices')/module/indices[module]['archiveSha256'][:12]))==member)
  item={'id':attr_id,'module':module,'sbomPackage':pid,'sourceAttribution':record['path'],'sourceAttributionSha256':record['sha256'],'decision':decision,'reason':reason,'metadata':value}
  if attr_id=='blake2' and features.get('system_libb2')==1:item['reason']+=' Installed system_libb2=1; retain attribution for the interface/header, not a claim that Qt compiled its bundled implementation.'
  if attr_id=='opengl-es2-headers' and features.get('opengles2')==-1:item['reason']+=' OpenGL ES2 is disabled; retain the header attribution conservatively.'
  if attr_id=='libdbus-1-headers':item['selectedLicense']='AFL-2.1'
  if attr_id=='grayraster':item['selectedLicense']='FTL'
  attributes.append(item)
  if decision=='include':required.append({'component':module,'path':record['path'],'ids':[attr_id]})
 assert {'webgradients','libdbus-1-headers'}<=set(a['id'] for a in attributes if a['decision']=='include')
 # The previous broad collection included Pixman, but the actual arm64 CMake
 # branch explicitly disables its assembly helpers; this is not absence-only.
 assert 'NOT (TEST_architecture_arch STREQUAL "arm64")' in gui
 pixman_record=next(r for r in raw if r['component']=='qtbase' and r['path'].endswith('/src/3rdparty/pixman/qt_attribution.json'))
 attributes.append({'id':'pixman','module':'qtbase','sbomPackage':None,'sourceAttribution':pixman_record['path'],'sourceAttributionSha256':pixman_record['sha256'],
  'decision':'excluded-platform','reason':'Gui CMake explicitly excludes Pixman assembly when TEST_architecture_arch is arm64; Qt arm64 NEON helpers are separate.',
  'metadata':json.loads(source('qtbase','src/3rdparty/pixman/qt_attribution.json'))})

 # Source conditions and positively defined local symbols establish PDFium's
 # static libraries; symbol absence alone is never used as an exclusion proof.
 pdf=symbols['QtPdf'];pdf_definitions='\n'.join(l for l in pdf.splitlines() if re.search(r' [tT] ',l))
 spec=[('pdfium','Qt 6.11.0 pinned snapshot','BSD-3-Clause and additional terms in original LICENSE',r'_FPDF_',PDFIUM),
  ('chromium-partitionalloc','Qt 6.11.0 Chromium snapshot','BSD-3-Clause',r'partition_alloc',PA),
  ('freetype','2.13.3 + revision 27c1cb10a52420515ce66729dfca897be21691b8','FTL',r'_FT_Init_FreeType',CHROMIUM+'third_party/freetype/'),
  ('libjpeg-turbo','3.1.0','IJG AND BSD-3-Clause AND Zlib',r'_chromium_jpeg_CreateDecompress',CHROMIUM+'third_party/libjpeg_turbo/'),
  ('icu','74.2','See ICU original MIT/Unicode/data terms',r'_u_tolower_74',CHROMIUM+'third_party/icu/'),
  ('abseil','56945519b5d17dcec0982ac6cdf4b6420f03c9c3','Apache-2.0',r'_ZN4absl',CHROMIUM+'third_party/abseil-cpp/'),
  ('fast-float','7.0.0 / cb1d42aaa1e14b09e1452cfdef373d051b8c02a4','MIT',r'_ZN10fast_float',CHROMIUM+'third_party/fast_float/'),
  ('agg','2.3','See original copying (permissive)',r'pdfium3agg',PDFIUM+'third_party/agg23/'),
  ('lcms','2.15 (LCMS_VERSION 2150)','MIT',r' _cms[A-Z]',PDFIUM+'third_party/lcms/'),
  ('openjpeg','2.5.3','BSD-2-Clause',r'_opj_create_decompress',PDFIUM+'third_party/libopenjpeg/')]
 pdf_items=[];prefixes=[CHROMIUM+'LICENSE',CHROMIUM+'base/third_party/icu/',PDFIUM+'LICENSE',PDFIUM+'AUTHORS']
 for identifier,version,license,pattern,prefix in spec:
  matches=[s for s in pdf_definitions.splitlines() if re.search(pattern,s)];assert matches,('Missing positive binary evidence',identifier)
  pdf_items.append({'id':identifier,'version':version,'license':license,'linkage':'static','sourcePrefix':prefix,'positiveSymbolCount':len(matches),'positiveSymbolExamples':matches[:3]})
  if identifier not in ['pdfium','chromium-partitionalloc']:prefixes.append(prefix)
 pdf_cmake=source('qtwebengine','src/pdf/CMakeLists.txt');toolchain=source('qtwebengine','cmake/QtToolchainHelpers.cmake')
 assert 'use_partition_alloc=true' in toolchain and 'use_allocator_shim=false' in toolchain
 assert 'ARGS pdf_enable_v8' in pdf_cmake and 'CONDITION QT_FEATURE_pdf_v8' in pdf_cmake
 conditions={}
 for name in ['BUILD.gn','pdfium.gni','core/fxcodec/BUILD.gn','core/fxcrt/BUILD.gn','core/fxge/BUILD.gn','third_party/BUILD.gn']:
  conditions[name]=source('qtwebengine',PDFIUM+name)
 source('qtwebengine','src/3rdparty/chromium/build_overrides/pdfium.gni')
 source('qtwebengine',PA+'src/partition_alloc/BUILD.gn')
 source('qtwebengine',PA+'src/partition_alloc/shim/allocator_shim_override_apple_symbols.h')
 assert 'pdf_use_skia_override = false' in source('qtwebengine','src/3rdparty/chromium/build_overrides/pdfium.gni')
 assert re.search(r' U _inflate$',pdf,re.M) and not re.search(r' [tT] _inflate$',pdf,re.M)
 assert re.search(r' U _png_create_read_struct$',pdf,re.M) and not re.search(r' [tT] _png_create_read_struct$',pdf,re.M)
 def covered(relative):return any(relative.startswith(p) if p.endswith('/') else relative==p for p in prefixes)
 def legal(relative):
  name=Path(relative).name.lower();return name.startswith(('license','licence','copying','copyright','notice','authors','patents')) or name in ['ftl.txt','readme.ijg']
 pdf_originals=[r for r in raw if r['component']=='qtwebengine' and r['sourceArchive']==indices['qtwebengine']['archive'] and legal(r['path']) and covered(str(Path(r['path']).relative_to(Path('notices/qtwebengine')/indices['qtwebengine']['archiveSha256'][:12])))]
 assert any(r['path'].endswith('/chromium/LICENSE') for r in pdf_originals)
 for record in pdf_originals:required.append({'component':'qtwebengine','path':record['path'],'ids':[]})
 # Preserve source-file copyright notices (e.g. Foxit font data) in addition to
 # the general project licenses. Conditional production headers are retained.
 groups=defaultdict(list)
 for prefix in [PDFIUM,PA]:
  for p in (roots['qtwebengine']/prefix).rglob('*'):
   rel=str(p.relative_to(roots['qtwebengine']))
   if not p.is_file() or p.suffix not in ['.c','.cc','.cpp','.h','.mm']:continue
   tail=rel.removeprefix(prefix)
   if any(s in tail.split('/') for s in ['third_party','testing','test','tests','samples','fuzzers','shim']) or '_unittest.' in tail or '_embeddertest.' in tail:continue
   header=leading_header(p.read_text(errors='replace'))
   if header:groups[header].append(rel)
 header_file=INPUTS/'review-notices/qtwebengine/PRODUCTION-SOURCE-COPYRIGHTS.txt';header_file.parent.mkdir(parents=True,exist_ok=True)
 header_file.write_text('Original leading copyright/license headers from PDFium and Chromium PartitionAlloc production source scopes.\nConditional source headers are retained; this is not a list of exact compiled translation units.\n\n'+''.join('Sources:\n'+'\n'.join(paths)+'\n\n'+header+'\n\n' for header,paths in sorted(groups.items())))
 generated={'component':'qtwebengine','path':'notices/qtwebengine/PRODUCTION-SOURCE-COPYRIGHTS.txt','sourceFile':str(header_file.relative_to(ROOT)),'sha256':sha(header_file)}
 report={'schemaVersion':1,'platform':'macOS-arm64','baseCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip(),
 'reviewCompleteForQtPayload':True,'allComponentsEmbeddedCodeReviewComplete':False,'releaseReady':False,
 'scope':'Selected Qt binaries, SBOM/source/feature conditions and positive symbol evidence. No original compile_commands or linker map claimed. Conditional/header notices are conservatively retained.',
 'qtBinaryCount':len(bindings),'frameworkCount':sum('.framework/' in b['path'] for b in bindings),'pluginCount':sum('/PlugIns/' in b['path'] for b in bindings),
 'originalQtBottleBuildEnvironments':receipt_builds,
 'binaryBindings':bindings,'sourceArchives':{k:v['archiveSha256'] for k,v in indices.items()},'evidenceFiles':evidence,
 'qtFeatureConditions':{k:v for k,v in features.items() if k in ['system_libb2','vulkan','opengles2','system_freetype','system_harfbuzz','system_jpeg','system_png','system_zlib','emojisegmenter']},
 'qtAttributions':attributes,'requiredOriginalNotices':required,'generatedOriginalHeaderNotices':[generated],
 'pdfium':{'staticComponents':pdf_items,'features':{k:v for k,v in features.items() if k.startswith('pdf_')},
 'dynamicDependencies':[{'component':'zlib','evidence':'QtPdf undefined inflate functions; .app contains libz.1.3.2 and resolves its dependency internally.'},{'component':'libpng','evidence':'QtPdf undefined png functions; .app contains libpng16.16 and resolves its dependency internally.'}],
 'excluded':[{'id':'v8-xfa-bmp-gif-png-tiff','reason':'Installed qtpdf config disables V8/XFA and their codec features; matching CMake forwards them to GN.'},{'id':'skia-fontations','reason':'PDFium build override disables Skia; no Fontations without Skia. AGG is positively defined.'},{'id':'apple-apsl-allocator-shim','reason':'Qt GN setup sets use_allocator_shim=false; pdfium_pa depends on PartitionAlloc/raw_ptr. APSL includes are inside the allocator shim; no shim symbol claim.'}],
 'originalHeaderGroups':len(groups),'originalHeaderSourceFiles':sum(map(len,groups.values()))},
 'remaining':['Other non-Qt shared-library embedded-code conditions and actual Rust compilation closure remain outside this Qt review.','Public corresponding-source delivery and formal release signing remain separate release checks.']}
 write(ROOT/'LICENSE/macOS/embedded-code-review.json',report)
 write(WORK/'review.json',report)
 print('Qt review:',len(bindings),'binaries;',len([a for a in attributes if a['decision']=='include']),'runtime attributions;',len(pdf_items),'PDFium static components;',len(groups),'original header groups',flush=True)


if __name__=='__main__':main()
