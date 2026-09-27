// UNIFY -> Unreal backend.
//
// STATUS: written against the UE5 API; not compiled in UNIFY's CI (no Unreal toolchain there).
// The interface it implements (unify::Backend) is the same one the SDL2 and headless backends
// implement and that CI exercises.
#pragma once

#include "CoreMinimal.h"
#include "Components/SynthComponent.h"
#include "runtime/backend.h"
#include "UnifyUnrealBackend.generated.h"

class UTexture2D;
class UCanvas;

/// UNIFY RenderDevice on Unreal: textures are transient UTexture2Ds; each UNIFY batch becomes one
/// FCanvasTriangleItem (so UNIFY's batching carries through to Unreal draw calls). Batches are
/// recorded during Engine::render() and replayed onto the HUD canvas by FlushToCanvas().
class UNIFYUNREAL_API FUnifyUnrealRenderDevice : public unify::RenderDevice
{
public:
    unify::TextureHandle create_texture(const unify::Image& Image) override;
    bool update_texture(unify::TextureHandle H, const unify::Image& Image) override;
    bool destroy_texture(unify::TextureHandle H) override;
    bool texture_size(unify::TextureHandle H, int& W, int& Hh) const override;
    void begin(unify::RenderTarget& Target, unify::Color Clear) override;
    void draw(const unify::Batch& Batch) override;
    void end() override {}
    uint32_t live_textures() const override { return Handles.alive(); }

    /// Draws the recorded frame, scaled to the canvas with integer pixel scaling.
    void FlushToCanvas(UCanvas* Canvas, float LogicalWidth, float LogicalHeight);
    void AddReferencedObjects(FReferenceCollector& Collector);

private:
    struct FRecordedBatch { UTexture2D* Texture; ESimpleElementBlendMode Blend; TArray<FCanvasUVTri> Tris; unify::RectI Scissor; };
    unify::HandlePool<unify::TextureHandle> Handles;
    TArray<TObjectPtr<UTexture2D>> Textures;   // indexed by handle slot; strong refs so GC keeps them
    TArray<FIntPoint> Sizes;
    TArray<FRecordedBatch> Frame;
    FLinearColor ClearColor = FLinearColor::Black;
    TObjectPtr<UTexture2D> White;
};

/// Audio: the UNIFY mixer renders directly inside Unreal's audio render thread.
UCLASS(ClassGroup = (UNIFY), meta = (BlueprintSpawnableComponent))
class UNIFYUNREAL_API UUnifySynthComponent : public USynthComponent
{
    GENERATED_BODY()
public:
    unify::Mixer* Mixer = nullptr;
protected:
    virtual bool Init(int32& SampleRate) override;
    virtual int32 OnGenerateAudio(float* OutAudio, int32 NumSamples) override;
};

class FUnifyUnrealAudioDevice : public unify::AudioDevice
{
public:
    explicit FUnifyUnrealAudioDevice(UUnifySynthComponent*& InSynth) : Synth(InSynth) {}
    bool start(unify::Mixer& Mixer) override;
    void stop() override;
    const char* name() const override { return "unreal"; }
private:
    UUnifySynthComponent*& Synth;
};

/// The backend object handed to unify::Engine.
class UNIFYUNREAL_API FUnifyUnrealBackend : public unify::Backend
{
public:
    FUnifyUnrealBackend() : Audio(Synth) {}
    const char* name() const override { return "unreal"; }
    bool init(const unify::EngineConfig& Config, std::string& Error) override;
    unify::RenderDevice& render_device() override { return Device; }
    unify::AudioDevice& audio_device() override { return Audio; }
    void poll(unify::InputSystem& Input, bool& Quit) override;
    void present(const unify::RenderTarget& Frame) override;
    uint64_t now_ns() override;
    void shutdown() override;

    /// Called from Unreal input callbacks (game thread); events are stamped on arrival.
    void QueueKey(const FKey& Key, bool bDown, bool bRepeat);
    void QueueAxis(const FKey& Key, float Value);
    void QueuePointer(float X, float Y);

    FUnifyUnrealRenderDevice Device;
    UUnifySynthComponent* Synth = nullptr;
    bool bQuitRequested = false;

private:
    FUnifyUnrealAudioDevice Audio;
    TArray<unify::InputEvent> Pending;
    FCriticalSection PendingLock;
};
