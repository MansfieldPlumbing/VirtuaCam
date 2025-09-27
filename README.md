.\build.ps1```

### 2. Register the Virtual Camera DLL (Administrator Required)

After a successful build, you must register the core COM server. This step requires Administrator privileges.

1.  Open **PowerShell** or **Command Prompt** as an **Administrator**.
2.  Navigate to the root directory of the VirtuaCam project where the build artifacts were copied.
3.  Run the following command:

    ```cmd
    regsvr32 DirectPortClient.dll
    ```

You should see a confirmation message that the DLL was registered successfully. You can also use the build script for this: `.\build.ps1 -Register`.

### 3. Run the VirtuaCam Controller

Double-click on `VirtuaCam.exe`. A new icon will appear in your system tray. This application runs the background broker process and provides the main user interface for controlling the camera.

### 4. Select a Video Source

Right-click the VirtuaCam tray icon to open the context menu.

*   **To share a window:** Go to `Source` -> `[Window Title]`. A producer process (`VirtuaCamProcess.exe`) will launch automatically to capture and broadcast that window's contents.
*   **To use a physical webcam:** Go to `Source` -> `[Webcam Name]`. A producer will launch to pass through your physical webcam feed.
*   **To use the auto-discovery grid:** Go to `Source` -> `Auto-Discovery Grid`. The camera will display a grid of all other active VirtuaCam-compatible producers running on your system.
*   **To add Picture-in-Picture:** Use the `Picture-in-Picture` sub-menu to select a source for the PIP overlay. You can enable additional PIP windows in the `Settings` menu.

### 5. Use in Your Target Application

Open an application like the **Windows Camera App**, **Zoom**, **Discord**, or **Microsoft Teams**. In the video settings, you should now be able to select **"VirtuaCam"** as your webcam. The feed you configured in the previous step will be displayed.

---

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.

## Acknowledgements

A special thank you to the developer of the **[VCamSample](https://github.com/smourier/VCamSample)** project. VCamSample provided an excellent and clear foundational example of a Media Foundation virtual camera. Its well-structured code served as an invaluable educational resource and a starting point for understanding the core concepts involved in this project.
