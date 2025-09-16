#pragma once

#include <string>
#include <vector>
#include <memory>
#include <cstdint>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

namespace DirectPort {

    struct BroadcastManifest {
        UINT64 frameValue;
        UINT width;
        UINT height;
        DXGI_FORMAT format;
        LUID adapterLuid;
        WCHAR textureName[256];
        WCHAR fenceName[256];
    };

    class DeviceD3D11;
    class DeviceD3D12;
    class Texture;
    class Consumer;
    class Producer;
    class Window;

    struct ProducerInfo {
        unsigned long pid;
        std::wstring executable_name;
        std::wstring stream_name;
        std::wstring type;
    };

    std::vector<ProducerInfo> discover();

    class Texture {
    public:
        ~Texture();
        uint32_t get_width() const;
        uint32_t get_height() const;
        DXGI_FORMAT get_format() const;
        uintptr_t get_d3d11_texture_ptr();
        uintptr_t get_d3d11_srv_ptr();
        uintptr_t get_d3d11_rtv_ptr();
        uintptr_t get_d3d12_resource_ptr();

    private:
        friend class DeviceD3D11;
        friend class DeviceD3D12;
        friend class Consumer;
        friend class Producer;
        Texture();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

    class Consumer {
    public:
        ~Consumer();
        bool wait_for_frame();
        bool is_alive() const;
        std::shared_ptr<Texture> get_texture();
        std::shared_ptr<Texture> get_shared_texture();
        unsigned long get_pid() const;
    private:
        friend class DeviceD3D11;
        friend class DeviceD3D12;
        Consumer();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

    class Producer {
    public:
        ~Producer();
        void signal_frame();
    private:
        friend class DeviceD3D11;
        friend class DeviceD3D12;
        Producer();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };
    
    class Window {
    public:
        ~Window();
        bool process_events();
        void present(bool vsync = true);
        void set_title(const std::string& title);
        uint32_t get_width() const;
        uint32_t get_height() const;
    private:
        friend class DeviceD3D11;
        friend class DeviceD3D12;
        Window();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };

    class IDirectXDevice {
    public:
        virtual ~IDirectXDevice() = default;

        virtual std::shared_ptr<Texture> create_texture(uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data = nullptr, size_t data_size = 0) = 0;
        virtual std::shared_ptr<Producer> create_producer(const std::string& stream_name, std::shared_ptr<Texture> texture) = 0;
        virtual std::shared_ptr<Consumer> connect_to_producer(unsigned long pid) = 0;
        virtual std::shared_ptr<Window> create_window(uint32_t width, uint32_t height, const std::string& title) = 0;
        virtual void resize_window(std::shared_ptr<Window> window) = 0;

        virtual void apply_shader(std::shared_ptr<Texture> output, const std::vector<uint8_t>& shader_bytes, const std::string& entry_point, const std::vector<std::shared_ptr<Texture>>& inputs, const std::vector<uint8_t>& constants) = 0;
        virtual void copy_texture(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination) = 0;
        virtual void blit(std::shared_ptr<Texture> source, std::shared_ptr<Window> destination) = 0;
        virtual void clear(std::shared_ptr<Window> window, float r, float g, float b, float a) = 0;
        
        virtual void blit_texture_to_region(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination,
                                            uint32_t dest_x, uint32_t dest_y, uint32_t dest_width, uint32_t dest_height) = 0;
    };

    class DeviceD3D11 : public IDirectXDevice, public std::enable_shared_from_this<DeviceD3D11> {
    public:
        static std::shared_ptr<DeviceD3D11> create();
        ~DeviceD3D11() override;

        std::shared_ptr<Texture> create_texture(uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data = nullptr, size_t data_size = 0) override;
        std::shared_ptr<Producer> create_producer(const std::string& stream_name, std::shared_ptr<Texture> texture) override;
        std::shared_ptr<Consumer> connect_to_producer(unsigned long pid) override;
        std::shared_ptr<Window> create_window(uint32_t width, uint32_t height, const std::string& title) override;
        void resize_window(std::shared_ptr<Window> window) override;

        void apply_shader(std::shared_ptr<Texture> output, const std::vector<uint8_t>& shader_bytes, const std::string& entry_point, const std::vector<std::shared_ptr<Texture>>& inputs, const std::vector<uint8_t>& constants) override;
        void copy_texture(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination) override;
        void blit(std::shared_ptr<Texture> source, std::shared_ptr<Window> destination) override;
        void clear(std::shared_ptr<Window> window, float r, float g, float b, float a) override;
        void blit_texture_to_region(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination,
                                    uint32_t dest_x, uint32_t dest_y, uint32_t dest_width, uint32_t dest_height) override;

    private:
        DeviceD3D11();
        struct Impl;
        std::unique_ptr<Impl> pImpl;
    };
    
    class DeviceD3D12 : public IDirectXDevice, public std::enable_shared_from_this<DeviceD3D12> {
    public:
        static std::shared_ptr<DeviceD3D12> create();
        ~DeviceD3D12() override;

        std::shared_ptr<Texture> create_texture(uint32_t width, uint32_t height, DXGI_FORMAT format, const void* data = nullptr, size_t data_size = 0) override;
        std::shared_ptr<Producer> create_producer(const std::string& stream_name, std::shared_ptr<Texture> texture) override;
        std::shared_ptr<Consumer> connect_to_producer(unsigned long pid) override;
        std::shared_ptr<Window> create_window(uint32_t width, uint32_t height, const std::string& title) override;
        void resize_window(std::shared_ptr<Window> window) override;
        
        void apply_shader(std::shared_ptr<Texture> output, const std::vector<uint8_t>& shader_bytes, const std::string& entry_point, const std::vector<std::shared_ptr<Texture>>& inputs, const std::vector<uint8_t>& constants) override;
        void copy_texture(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination) override;
        void blit(std::shared_ptr<Texture> source, std::shared_ptr<Window> destination) override;
        void clear(std::shared_ptr<Window> window, float r, float g, float b, float a) override;
        void blit_texture_to_region(std::shared_ptr<Texture> source, std::shared_ptr<Texture> destination,
                                    uint32_t dest_x, uint32_t dest_y, uint32_t dest_width, uint32_t dest_height) override;

    private:
        DeviceD3D12();
        struct Impl;
        void WaitForGpu();
        std::unique_ptr<Impl> pImpl;
    };
}