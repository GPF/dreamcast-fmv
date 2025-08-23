# Dreamcast FMV Player

This is a proof-of-concept FMV (Full Motion Video) playback toolchain for the Sega Dreamcast.
It includes:

* `pack_dcmv`: a frame+audio packer using lz4 HClevel 12 , or zstd level 13 compression
* `fmv_play.elf`: a Dreamcast player that decompresses and displays the video while streaming synced ADPCM audio
* conversion tools using ffmpeg + `pvrtex` + `dcaconv`

## Goals

* Minimize CPU usage on Dreamcast by pre-processing all video/audio data
* Leverage hardware VQ-compressed textures and sound streaming for smooth playback

## Project Structure

```
.
├── convert_to_pvr_fmv.sh       # Main conversion script (edit manually to configure input)
├── dcaconv                     # ADPCM encoder (built from TapamN's dcaconv repo)
├── pack_dcmv.c                 # Source for video+audio packer
├── pack_dcmv                   # Compiled binary (use: `gcc -O2 pack_dcmv.c -o pack_dcmv -llz4 -lzstd`)
├── generate_durations.py       # python3 script with PIL and ImageChops, numpy to find similiar png exported from ffmpeg and generate a repeat count of same frame to reduce size of dcmv file.
├── input/
│   └── Your source .mp4 files (manually configured in convert_to_pvr_fmv.sh)
├── playdcmv/
│   ├── fmv_play.c             # Dreamcast playback code (uses lz4, PVR, snd_stream)
│   ├── fmv_play.elf           # Compiled player binary
└── └── movie.dcmv             # Final Dreamcast FMV file

```

## Dependencies

* **ffmpeg**: used to extract RGB64be png frames and audio from video files.
* **pvrtex**: builds VQ-compressed YUV422/RGB565 Dreamcast textures (from KOS utils folder)
* **python3**:  python3 script with PIL and ImageChops, numpy to find similiar png exported from ffmpeg and generate a repeat count of same frame to reduce size of dcmv file.
* **dcaconv**: encodes WAV audio to Dreamcast ADPCM format ([https://github.com/TapamN/dcaconv](https://github.com/TapamN/dcaconv))
* **lz4**: used for lz4 compression [([https://github.com/GPF/lz4](https://github.com/GPF/lz4))](https://github.com/GPF/lz4) //for speed over size.
* **zstd**: used for zstd compression [([https://github.com/GPF/zstd](https://github.com/GPF/zstd))](https://github.com/GPF/zstd) //for size over speed.

## Usage

1. Edit `convert_to_pvr_fmv.sh`:

   * Set the input video path (e.g., `input/Dreamcast Startup (60fps).mp4`) 
   ([https://archive.org/details/dreamcaststartup60fps])
   * Adjust parameters like width, height, scalewidth, scaleheight, strided/pot texture size, FPS, compression type(lz4 or zstd),remove similiar frames, sample rate, channels, and audio bitrate
2. Run the script:

   ```bash
   ./convert_to_pvr_fmv.sh
   ```
3. Burn the resulting `movie.dcmv` + `fmv_play.elf` (or `dcmv.cdi`) to a disc or run with an emulator like Flycast or on real hardware.

## License

This project is for educational/demo purposes. See individual tools for respective licenses.

## Author

Troy E. Davis ([@GPF](https://github.com/GPF)) – Dreamcast homebrew hacker, QA engineer, and SDL3 port contributor.

---

🎥 Screenshots and sample clips coming soon!



https://github.com/user-attachments/assets/60f02ab9-aaa7-48cc-96fd-22c802bc4e7f

📦 Header v6: YUV422 640x480 (content: 640x480) @ 23.97fps, 44100Hz, 2ch, unique=1039, total=1225
   Frame size: 78848, Max compressed: 60789, Audio offset: 0x2B32AD6, Compression: LZ4
vid_set_mode: 640x480 VGA with 1 framebuffers.
🔄 Loading initial frames synchronously...
✅ Starting playback @ 41.72ms/frame, total=1225, unique=1039
🏁 Playback finished

arch: exit return code 0
arch: shutting down kernel
vid_set_mode: 640x480 VGA with 1 framebuffers.
gpf@GPF:~/code/dreamcast/dreamcast-fmv/playdcmv$ ls -la movie.dcmv 
-rw-r--r-- 1 gpf gpf 47618230 Aug 22 20:33 movie.dcmv
gpf@GPF:~/code/dreamcast/dreamcast-fmv/playdcmv$

![image](https://github.com/user-attachments/assets/6e24fbb8-2f86-4c95-a097-b26e14f6b521)


https://github.com/user-attachments/assets/60b07856-052f-4854-9005-18eb9ce9aa38


https://github.com/user-attachments/assets/54731601-5f5e-420a-be52-f8ba33d16adc


https://github.com/user-attachments/assets/5ef68680-6b25-4d52-a357-63b1aa6d9c12

DirkSimple fmv video used with https://github.com/GPF/dirksimple port


https://github.com/user-attachments/assets/7460e88a-bf33-4bbf-991d-49a049922172



https://github.com/user-attachments/assets/f23d55a9-d797-44bd-9f86-f2aeac5dddef




Feel free to fork, test on hardware, or suggest features.
