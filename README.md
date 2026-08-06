# NGSVM -- Non-Green Screen Video Matting

An Unreal Engine 5.7 plugin for real-time AI matting (background removal), built on UE's NNE (Neural Network Engine). Runs RVM (Robust Video Matting) or MODNet models on CPU or GPU (DirectML) to key a live video/camera feed or a single static image, without needing a green screen.

## Features

- No green screen required -- matting works directly on ordinary footage of a person in a normal environment
- Real-time video/camera matting via a drop-in `UNGSVMManager` actor component
- Static image matting, both synchronous (`NGSVM_KeyImage_asTexture2D`) and non-blocking async (`NGSVM_KeyImageAsync`) Blueprint functions, each returning a keyed result plus a standalone alpha matte
- Composure integration: a modern CompositeCore pass and a Legacy Composure-compatible pass, so AI matting can run directly inside a Composure compositing graph
- Switchable CPU / GPU (DirectML) inference, with FP16 and FP32 variants of each supported model
- Adjustable inference resolution (`Resolution Scale`) independent of output resolution, so matting quality/performance can be tuned without affecting the final output size

## Plugins

This repo currently ships two plugins:

- **NGSVMCore** -- the primary, self-contained implementation. Model loading, the RVM/MODNet inference pipelines, `UNGSVMManager`, and the static-image keying functions all live here. Has no dependency on Composure or CompositeCore.
- **NGSVMComposure** -- integrates NGSVMCore's matting into UE5.7's Composure and CompositeCore compositing systems. Depends on NGSVMCore; only needed if you're compositing through Composure rather than using `UNGSVMManager` directly.

<p align="center">
  <img width="854" height="480" alt="NGSVM-Composure-Demo" src="https://github.com/user-attachments/assets/c1eb87e0-52f4-4939-9589-26b91ee12809" />
</p>


> Both plugins are published together in this repo. **NGSVMComposure always requires NGSVMCore** to be present -- if you only need real-time/static-image matting without Composure, you can simply leave the `NGSVMComposure` plugin disabled.

## Requirements

- Unreal Engine 5.7 (NGSVMComposure's CompositeCore-based pass relies on APIs introduced in 5.7; see the Known Limitations section)
- Windows (GPU inference uses DirectML via `NNERuntimeORTDml`, which is Windows-only; CPU inference via `NNERuntimeORTCpu` has no such restriction)
- RVM and/or MODNet model weights, converted to `.onnx` and imported into the project as `UNNEModelData` assets (not included in this repo -- see Setup below)

## Installation

1. Copy `Plugins/NGSVMCore` into your project's `Plugins/` folder. Copy `Plugins/NGSVMComposure` too if you need the Composure integration.
2. Enable both plugins in the Unreal Editor (**Edit > Plugins**) and restart the editor if prompted.
3. Confirm the **NNE**, **EnhancedInput**, and (if using NGSVMComposure) **Composure** / **Composite** / **CompositeCore** plugins are enabled -- these are declared as dependencies and should enable automatically.

## Setup

1. Download the ONNX model file(s) you need (see below), or convert your own trained weights to ONNX, then import each `.onnx` file into the project as a `UNNEModelData` asset.
2. Open **Project Settings > Plugins > NGSVM Core**, and assign each imported model asset to its matching slot (`rvm_mobilenetv3_fp16`, `rvm_mobilenetv3_fp32`, `rvm_resnet50_fp16`, `rvm_resnet50_fp32`, `modnet`). Only the slots you actually plan to use need to be filled in.

### Model downloads

Not redistributed in this repo -- see [Model Licensing](#model-licensing) below before using these. Links are hosted by the respective upstream projects, not by NGSVM.

**RVM (Robust Video Matting)** -- direct ONNX downloads from the [official v1.0.0 release](https://github.com/PeterL1n/RobustVideoMatting/releases/tag/v1.0.0):

| Model | Download |
|---|---|
| RVM MobileNetV3 FP32 | [rvm_mobilenetv3_fp32.onnx](https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp32.onnx) |
| RVM MobileNetV3 FP16 | [rvm_mobilenetv3_fp16.onnx](https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_mobilenetv3_fp16.onnx) |
| RVM ResNet50 FP32 | [rvm_resnet50_fp32.onnx](https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_resnet50_fp32.onnx) |
| RVM ResNet50 FP16 | [rvm_resnet50_fp16.onnx](https://github.com/PeterL1n/RobustVideoMatting/releases/download/v1.0.0/rvm_resnet50_fp16.onnx) |

**MODNet** -- no FP16/FP32 split upstream; the official repo doesn't publish a ready-made ONNX file directly in the repo, but does link one:

- Pre-exported ONNX (Google Drive, linked from [MODNet/onnx/README.md](https://github.com/ZHKKKe/MODNet/blob/master/onnx/README.md)): [modnet.onnx](https://drive.google.com/file/d/1cgycTQlYXpTh26gB9FTnthE7AvruV8hd/view?usp=sharing)
- Or export it yourself: download the pretrained checkpoint from the [MODNet pretrained models folder](https://drive.google.com/drive/folders/1umYmlCulvIFNaqPjwod1SayFmSRHziyR?usp=sharing), then run `python -m onnx.export_onnx --ckpt-path=pretrained/modnet_photographic_portrait_matting.ckpt --output-path=pretrained/modnet_photographic_portrait_matting.onnx` from the [MODNet repo](https://github.com/ZHKKKe/MODNet) (see `onnx/README.md` for exact dependency versions).

## Usage

### Real-time video matting

Add a `NGSVM Manager` component to any Actor/Pawn. Set `Input Render Target` to the source video/camera Render Target (optionally via `Input Material`, drawn to the target automatically every frame), pick a `Model Type` and `Execution Device`, then call `Start NGSVM Service`. The component exposes `Output Mask Texture`/`Output Color Texture` (and matching optional Render Target overrides) with the keyed result and alpha matte, updated in real time.

### Composure integration

Add either `NGSVM Composite Pass` (CompositeCore) or `NGSVM Legacy Composure Pass` (classic Composure) as a transform pass in your compositing graph, configure `Model Type`/`Execution Device`/`Resolution Scale`, and enable the pass. Settings and behavior are kept in parity between the two.

### Static image matting

Call `NGSVM_KeyImage_asTexture2D` (blocking) or `NGSVM_KeyImageAsync` (non-blocking, recommended for in-game use) from Blueprint or C++ to key a single `UTexture2D`. Both return a keyed result texture (straight alpha) and a standalone grayscale alpha matte texture, with matching parameter names/order.

## Performance

Benchmarked with `NGSVM Manager` on a 1920x1080 source video, measured via Unreal Engine's built-in `StartFPSChart` / `StopFPSChart`.

**Test environment**: NVIDIA GeForce RTX 5060 Laptop GPU, AMD Ryzen 9 7945HX, Windows 11, viewport 1280x720.

### CPU (ONNX Runtime CPU Provider)

| Model | 1x | 1/2 | 1/4 | 1/8 |
|---|---|---|---|---|
| RVM MobileNet V3 FP16 | 1.3 | 3.71 | 12.48 | 25.55 |
| RVM MobileNet V3 FP32 | 2.13 | 6.5 | 17.54 | 31.78 |
| RVM ResNet50 FP16 | 0.57 | 2.08 | 5.75 | 11.65 |
| RVM ResNet50 FP32 | 0.9 | 3.14 | 10.84 | 20.85 |
| MODNet | 1.6 | 5.91 | 15.81 | 33.88 |

> **Note:** FP16 models run *slower* than FP32 on CPU. x86 CPUs have no native FP16 execution units, so FP16 tensors are upcast to FP32 before computation, adding conversion overhead on top of the same underlying FP32 math. Prefer FP32 models when targeting CPU.

### GPU (DirectML)

| Model | 1x | 1/2 | 1/4 | 1/8 |
|---|---|---|---|---|
| RVM MobileNet V3 FP16 | 37.95 | 61.72 | 74.54 | 78.39 |
| RVM MobileNet V3 FP32 | 38.34 | 67.83 | 76.07 | 80.07 |
| RVM ResNet50 FP16 | 40.86 | 65.87 | 74.17 | 80.33 |
| RVM ResNet50 FP32 | 25.26 | 57.51 | 72.73 | 79.28 |
| MODNet | 39.83 | 74.28 | 80.3 | 85.33 |

### Known limitation: GPU inference currently occupies the Render Thread

At low Resolution Scale (1/4, 1/8), every model converges to a similar FPS ceiling (roughly 74-85 FPS) regardless of how lightweight the model is. This is a known architectural limitation, not measurement noise.

`UNGSVMManager` dispatches GPU inference via `ENQUEUE_RENDER_COMMAND`. This is required for correctness -- DirectML/D3D12 command submission is not thread-safe from an arbitrary background thread, and calling `IModelInstanceRunSync::RunSync()` off the Render Thread crashes inside the GPU driver. The side effect is that `RunSync()`, a blocking call, then occupies the Render Thread's single command queue for the entire duration of inference, stalling frame presentation until it completes -- even though the GPU hardware itself sits idle for most of that window. In one profiling capture: average GPU frametime was ~5.77 ms, while average RenderThread frametime was ~34 ms for the same frames.

The architecturally correct fix is to dispatch inference through NNE's RDG-integrated path (`IModelInstanceRDG` / `EnqueueRDG()`) instead of `IModelInstanceRunSync::RunSync()`, so inference is scheduled into the render graph rather than executed synchronously on the Render Thread. This has been confirmed feasible (`NNERuntimeORTDml` supports `EnqueueRDG()` via `INNERuntimeRDG`) but is not yet implemented. The GPU numbers above, especially at low Resolution Scale where inference itself is cheap, likely understate what's achievable once this is addressed.

## Other Known Limitations

- **No CUDA support.** UE's stock `NNERuntimeORT` plugin only ever registers `NNERuntimeORTDml` (DirectML) and `NNERuntimeORTCpu` -- there is no CUDA execution provider without integrating a separate third-party ONNX Runtime build. `Execution Device` is limited to CPU and GPU (DirectML) accordingly.
- **GPU inference is Windows-only.** DirectML is a Windows-only API; on other platforms only CPU inference is available.
- **`NGSVM Composite Pass` (CompositeCore) requires UE 5.7.** CompositeCore was introduced in 5.7; the Legacy Composure pass (`NGSVM Legacy Composure Pass`) has no such restriction and should work on older Composure-only versions of UE, though this hasn't been verified against a 5.4-5.6 build.

## Model Licensing

This repo does not include any model weights. RVM and MODNet each have their own upstream license terms, separate from this plugin's own license:

- **RVM (Robust Video Matting)** -- [PeterL1n/RobustVideoMatting](https://github.com/PeterL1n/RobustVideoMatting) is licensed under **[GPL-3.0](https://github.com/PeterL1n/RobustVideoMatting/blob/master/LICENSE)**, a strong copyleft license. If you convert and redistribute RVM's weights (or code derived from that repo) as part of your own release, GPL-3.0's terms likely require the distributed work to also be GPL-3.0-compatible -- confirm that's acceptable for your use case before bundling RVM weights.
- **MODNet** -- [ZHKKKe/MODNet](https://github.com/ZHKKKe/MODNet) is licensed under the **[Apache License 2.0](https://github.com/ZHKKKe/MODNet/blob/master/LICENSE)**, a permissive license (attribution + license notice required, no copyleft obligation).

Neither of these licenses is inherited automatically by this plugin's own code, which only loads externally-provided `.onnx` models at runtime and contains no RVM/MODNet source or weights -- see NGSVMRVMPipeline's implementation, which only talks to RVM's published tensor input/output contract (names, shapes) rather than reproducing any of RVM's own Python/PyTorch source. But those upstream licenses do apply to whatever model files *you* choose to convert, include, and redistribute alongside this plugin.

## License

NGSVM's own source code is licensed under the **[MIT License](LICENSE)** -- permissive, no copyleft obligation, free for any use including commercial.

This is a deliberate choice distinct from RVM's own GPL-3.0 license (see Model Licensing above): Unreal Engine's own EULA prohibits combining Engine code with GPL-licensed material, so a UE plugin -- which compiles directly against Engine modules -- cannot itself be GPL-3.0 licensed without violating that EULA. MIT keeps this plugin's own code fully compliant with the Engine EULA regardless of which model you choose to run through it.

## Acknowledgments

NGSVM builds entirely on top of two open-source matting models developed and maintained by their own authors -- huge thanks to them for releasing this work:

- **[RVM (Robust Video Matting)](https://github.com/PeterL1n/RobustVideoMatting)** by Shanchuan Lin, Linjie Yang, Imran Saleemi, and Soumyadip Sengupta -- ["Robust High-Resolution Video Matting with Temporal Guidance"](https://arxiv.org/abs/2108.11515) (WACV 2022).
- **[MODNet](https://github.com/ZHKKKe/MODNet)** by Zhanghan Ke, Jiayu Sun, Kaican Li, Qiong Yan, and Rynson W.H. Lau -- ["MODNet: Real-Time Trimap-Free Portrait Matting via Objective Decomposition"](https://arxiv.org/abs/2011.11961) (AAAI 2022).
