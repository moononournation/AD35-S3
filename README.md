# AD35-S3

## conversion

### Cinepak

```sh
ffmpeg -i input.mp4 -c:a mp3 -c:v cinepak -q:v 40 -vf "fps=30,scale=-1:320:flags=lanczos,crop=480:320:(in_w-480)/2:0" AviMp3Cinepak320p30fps.avi
```

### MJPEG

```sh
ffmpeg -i input.mp4 -c:a mp3 -c:v mjpeg -q:v 10 -vf "fps=15,scale=-1:320:flags=lanczos,crop=480:320:(in_w-480)/2:0" AviMp3Mjpeg320p15fps.avi
```
