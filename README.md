# FaceOn
## Real-Time Face Studio with Virtual Camera Support

Face On is a high-performance, real-time face swapping and management application for Windows. It allows you to swap faces from your image library onto a live webcam feed or create entirely new personas by blending and morphing between different source faces in real-time.

It's designed as a stable and powerful control panel for anyone who wants to reliably manage and fine-tune their digital appearance for streaming, video calls, or creative projects.

<p align="center">
  <img src="https://github.com/user-attachments/assets/eb02ff1a-2e23-40cb-b50c-ae795f2572d6" alt="FaceOn Application Screenshot 1" width="45%"/>
  <img src="https://github.com/user-attachments/assets/2e9e68da-a7dc-4c62-987b-c3d1448b83a7" alt="FaceOn Application Screenshot 2" width="45%"/>
</p>

## Features

*   **Real-Time Swapping**: Instantly swap faces from your source images onto a live webcam feed.
*   **Face Blending**: Use a simple slider to seamlessly blend between two selected source faces, creating a unique hybrid.
*   **Automated Morphing**: Activate a continuous, smooth morph between all of the faces in your source library.
*   **EMAP Mode**: Don't have source images? Use the built-in "Emap Archetype" face to get started immediately.
*   **Fine-Tuning Control Panel**: A full suite of sliders lets you adjust the ROI (Region of Interest) margin, mask feathering, core tightness, and affine "nudge" controls (X/Y offset, scale) for a perfect blend.
*   **DirectPort Native Output**: Broadcasts its output using DirectPort, ensuring low-latency and seamless integration with virtual camera software like VirtuaCam.
*   **GPU Accelerated**: Utilizes ONNX Runtime with the DirectML execution provider for high-performance, hardware-accelerated processing on modern Windows systems.

## A Tale of Two Tools

Face On is one-half of a two-part release. It is the stable, control-focused tool designed for reliable performance and fine-tuning. If you want a control panel to manage, blend, and perfect face swaps, you are in the right place.

Its creative counterpart is **PaintShop Studio**, an experimental tool that lets you paint a source face in real-time and wear it like a digital mask. If you want an art canvas for creating surreal and bizarre effects, check out PaintShop Studio.

---

## PaintShop Studio: A Real-Time FaceSwap Playground

PaintShop Studio is an experimental, real-time face-swapping tool for Windows. Its core feature is a "paint shop" interface that lets you dynamically edit your source face image with a brush and instantly see the results applied to your own face on a live webcam feed.

It's not a polished "makeup booth." It's a weird, digital Cronenberg machine. If you've ever wanted to paint a new face onto a historical figure and then wear it in a video call, this is for you.

### Key Features
*   **Live Source Painting**: The main attraction. Load a source face, then paint, smudge, and modify it in real-time. Every brush stroke on the source image is reflected in the live face-swap.
*   **DirectPort Native Output**: Designed specifically for the Windows ecosystem. It broadcasts its output via DirectPort, making it instantly compatible with virtual camera software like [**VirtuaCam**](https://github.com/MansfieldPlumbing/VirtuaCam/releases/tag/VirtuaCam). This allows for low-latency use in OBS, Discord, Zoom, or any other application that accepts a webcam input.
*   **GPU Accelerated**: Uses ONNX Runtime with the DirectML execution provider for hardware-accelerated performance on modern Windows systems.
*   **Simple & Focused**: No complex menus or configuration. Just load an image and start painting.

### The Vibe (Managing Expectations)

I initially thought this could be a neat virtual makeup tool. It is not. The results are often surreal, uncanny, and artistically strange. The strength of this tool lies in its experimental nature. It's for creating bizarre effects, crafting uncanny new personas for a stream, or just exploring the strange side of AI face-swapping.

---

## Installation

### Prerequisites:

*   Windows 10 or 11
*   A DirectML-compatible GPU (most modern AMD, NVIDIA, or Intel GPUs)
*   Python 3.8+

### Instructions:

1.  **Clone the repository:**
    ```bash
    git clone https://github.com/MansfieldPlumbing/faceon.git
    cd face-on
    ```

2.  **Set up a Python virtual environment (recommended):**
    ```bash
    python -m venv venv
    .\venv\Scripts\activate
    ```

3.  **Install the required packages:**
    ```bash
    pip install -r requirements.txt
    ```

4.  **Download the Models:**
    The required ONNX model files are not included in the repository. They must be downloaded from the project's releases page.
    a. Go to the **Releases Page**.
    b. Under the "Assets" section of the latest release, download the `models.zip` file.
    c. Create a folder named `models` in the root of your project directory.
    d. Unzip the contents of `models.zip` directly into the `models` folder.

    Your final folder structure should look like this:
    ```
    face-on/
    ├── models/
    │   ├── det_10g.onnx
    │   ├── inswapper_128.onnx
    │   └── w600k_r50.onnx
    ├── faceonmain.py
    └── ... (etc.)
    ```

5.  **Compile or Download the DirectPort Extension:**
    This project requires a compiled extension for low-latency video.

    *   **Option A: Use the Pre-Compiled Version (Recommended)**
        For convenience, a pre-compiled version for 64-bit Windows and Python 3.10 is available.
        1.  Go to the [**Releases Page**](https://github.com/your-username/your-repo/releases).
        2.  Under "Assets", download the `.pyd` file (e.g., `directport.cp310-win_amd64.pyd`).
        3.  Place this file in the root directory of the project, next to `faceonmain.py`.

    *   **Option B: Compile from Source (Advanced)**
        If the pre-compiled version is not compatible with your system (e.g., you use Python 3.11), you can compile it yourself.
        1.  Install the [Microsoft C++ Build Tools](https://visualstudio.microsoft.com/visual-cpp-build-tools/).
        2.  Navigate to the `directport` directory in your terminal: `cd directport`
        3.  Run the build command: `python setup.py install`
        4.  This will compile and install the `.pyd` file into your Python environment.

---

## How to Use

### FaceOn

1.  **Add Source Images**: Place any face images (e.g., `.jpg`, `.png`) you want to use into the `/sources` folder. The application will automatically find them when it starts.
2.  **Run the Application**:
    ```bash
    python faceon.py
    ```
3.  **Control the UI**:
    *   **Mode Selection**: Use the radio buttons on the left to select your desired mode (Swap, Blend, Morph, etc.).
    *   **Source Selection**: Hover your mouse cursor over the source image thumbnails and use the mouse scroll wheel to cycle through your library. In **Blend** mode, you can select which thumbnail (A or B) is the active scroll target by clicking on it.
    *   **Adjust Sliders**: Use the sliders in the bottom half of the window to fine-tune the face-swapping parameters in real-time.
4.  **View the Live Output**: To see the results, you need a DirectPort viewer like [**VirtuaCam**](https://github.com/MansfieldPlumbing/VirtuaCam/releases/tag/VirtuaCam).

> **I highly recommend downloading my [VirtuaCam](https://github.com/MansfieldPlumbing/VirtuaCam/releases/tag/VirtuaCam) virtual camera so that you will have broadcasting abilities.**

### PaintShop Studio

1.  **Run the application**:
    ```bash
    python paintshop.py
    ```2.  **Load a Source Image**: Click the "Load Image" button and select an image file containing a face.
3.  **Start Painting**: Use the sliders to adjust brush size, hardness, and opacity. Click the color swatch to change colors. Paint directly on the main canvas.

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.
