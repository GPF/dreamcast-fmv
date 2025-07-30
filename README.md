# Dreamcast FMV Player

This is a proof-of-concept FMV (Full Motion Video) playback toolchain for the Sega Dreamcast.
It includes:

* `pack_dcmv`: a frame+audio packer using lz4 HClevel 12 compression
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
├── input/
│   └── Your source .mp4 files (manually configured in convert_to_pvr_fmv.sh)
├── playdcmv/
│   ├── fmv_play.c             # Dreamcast playback code (uses lz4, PVR, snd_stream)
│   ├── fmv_play.elf           # Compiled player binary
└── └── movie.dcmv             # Final Dreamcast FMV file

```

## Dependencies

* **ffmpeg**: used to extract YUV frames and audio from MP4
* **pvrtex**: builds VQ-compressed RGB565 Dreamcast textures (from KOS utils folder)
* **dcaconv**: encodes WAV audio to Dreamcast ADPCM format ([https://github.com/TapamN/dcaconv](https://github.com/TapamN/dcaconv))
* **lz4**: used for lz4 compression [([https://github.com/GPF/lz4](https://github.com/GPF/lz4))](https://github.com/GPF/lz4)

## Usage

1. Edit `convert_to_pvr_fmv.sh`:

   * Set the input video path (e.g., `input/Dreamcast Startup (60fps).mp4`) 
   ([https://archive.org/details/dreamcaststartup60fps])
   * Adjust parameters like width, height, FPS, sample rate, channels, and audio bitrate
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

![image](https://github.com/user-attachments/assets/6e24fbb8-2f86-4c95-a097-b26e14f6b521)


https://github.com/user-attachments/assets/60b07856-052f-4854-9005-18eb9ce9aa38


https://github.com/user-attachments/assets/54731601-5f5e-420a-be52-f8ba33d16adc


https://github.com/user-attachments/assets/5ef68680-6b25-4d52-a357-63b1aa6d9c12

DirkSimple fmv video used with https://github.com/GPF/dirksimple port


https://github.com/user-attachments/assets/7460e88a-bf33-4bbf-991d-49a049922172



https://github.com/user-attachments/assets/f23d55a9-d797-44bd-9f86-f2aeac5dddef




Feel free to fork, test on hardware, or suggest features.
