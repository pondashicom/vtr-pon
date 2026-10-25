#!/usr/bin/env python3
"""Create short authored media fixtures with a development-only encoder tool.

The encoder executable and its libraries are not copied into the test runtime.
Playback must independently prove its loaded FFmpeg libraries are the LGPL build.
"""
import argparse,hashlib,json,subprocess
from pathlib import Path


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output',type=Path,required=True)
    parser.add_argument('--ffmpeg',default='/opt/homebrew/bin/ffmpeg')
    args=parser.parse_args(); output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    tone='aevalsrc=0.1*sin(2*PI*440*t)|0.2*sin(2*PI*880*t):s=48000:d=1'
    cases=[
      ('mp4-h264-aac.mp4','h264_videotoolbox','aac',[],True),
      ('mp4-hevc-aac.mp4','hevc_videotoolbox','aac',['-tag:v','hvc1'],True),
      ('mov-prores-pcm.mov','prores_ks','pcm_s16le',['-pix_fmt','yuv422p10le','-profile:v','2'],True),
      ('mov-h264-pcm.mov','h264_videotoolbox','pcm_s16le',[],True),
      ('webm-vp8-vorbis.webm','libvpx','vorbis',['-deadline','realtime','-strict','-2'],True),
      ('webm-vp9-opus.webm','libvpx-vp9','libopus',['-deadline','realtime'],True),
      ('webm-av1-opus.webm','libsvtav1','libopus',['-preset','12'],True),
      ('mkv-h264-flac.mkv','h264_videotoolbox','flac',[],True),
      ('avi-mjpeg-pcm.avi','mjpeg','pcm_s16le',['-pix_fmt','yuvj420p'],True),
      ('mxf-mpeg2-pcm.mxf','mpeg2video','pcm_s16le',['-pix_fmt','yuv422p','-b:v','50000000','-minrate','50000000','-maxrate','50000000','-bufsize','17825792'],True),
      ('audio-pcm.wav',None,'pcm_s24le',[],False),
      ('audio-mp3.mp3',None,'libmp3lame',[],False),
      ('audio-flac.flac',None,'flac',[],False),
      ('audio-aac.m4a',None,'aac',[],False),
      ('audio-adts.aac',None,'aac',[],False),
      ('audio-vorbis.ogg',None,'vorbis',['-strict','-2'],False),
      ('audio-opus.ogg',None,'libopus',[],False),
      ('audio-pcm.aiff',None,'pcm_s24be',[],False),
      ('audio-alac.m4a',None,'alac',[],False),
    ]
    records=[]
    for filename,video,audio,extra,has_video in cases:
        path=output/filename
        cmd=[args.ffmpeg,'-nostdin','-hide_banner','-loglevel','error','-y']
        if has_video:
            size='1920x1080' if filename.endswith('.mxf') else '320x180'
            cmd+=['-f','lavfi','-i','color=c=0xe62020:s='+size+':r=25:d=1']
        cmd+=['-f','lavfi','-i',tone,'-t','1','-threads','2']
        if has_video:cmd+=['-c:v',video,'-pix_fmt','yuv420p']
        cmd+=extra+['-c:a',audio,'-ar','48000','-ac','2',str(path)]
        result=subprocess.run(cmd,capture_output=True,text=True,timeout=45)
        if result.returncode:
            raise RuntimeError(filename+': '+result.stderr)
        records.append({'file':filename,'hasVideo':has_video,'hasAudio':True,
          'videoCodec':video,'audioCodec':audio,'durationMs':1000,
          'sha256':hashlib.sha256(path.read_bytes()).hexdigest()})
        print('CREATED',filename,flush=True)
    (output/'fixtures.json').write_text(json.dumps({'authored':'solid red video and independent 440/880 Hz stereo sine; no imported media',
      'encoderTool':args.ffmpeg,'encoderToolIsDevelopmentOnly':True,'cases':records},indent=2)+'\n')


if __name__=='__main__':main()
