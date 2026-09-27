// STATUS: written against the UE5 API; not compiled in UNIFY's CI. See UnifyUnrealBackend.h.
#include "UnifyUnrealBackend.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "CanvasItem.h"
#include "HAL/PlatformTime.h"
#include "InputCoreTypes.h"

// ---------------------------------------------------------------------------------- render device

static UTexture2D* MakeTexture(const unify::Image& Image)
{
    UTexture2D* Tex = UTexture2D::CreateTransient(Image.width, Image.height, PF_R8G8B8A8);
    Tex->Filter = TF_Nearest;             // pixel art
    Tex->SRGB = true;
    Tex->CompressionSettings = TC_EditorIcon;
    Tex->UpdateResource();
    const int32 Bytes = Image.width * Image.height * 4;
    uint8* Copy = static_cast<uint8*>(FMemory::Malloc(Bytes));
    FMemory::Memcpy(Copy, Image.pixels.data(), Bytes);
    auto* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, Image.width, Image.height);
    Tex->UpdateTextureRegions(0, 1, Region, Image.width * 4, 4, Copy,
        [](uint8* Data, const FUpdateTextureRegion2D* R) { FMemory::Free(Data); delete R; });
    return Tex;
}

unify::TextureHandle FUnifyUnrealRenderDevice::create_texture(const unify::Image& Image)
{
    unify::TextureHandle H = Handles.allocate();
    const int32 Slot = int32(H.index());
    if (Slot >= Textures.Num()) { Textures.SetNum(Slot + 1); Sizes.SetNum(Slot + 1); }
    Textures[Slot] = MakeTexture(Image);
    Sizes[Slot] = FIntPoint(Image.width, Image.height);
    return H;
}

bool FUnifyUnrealRenderDevice::update_texture(unify::TextureHandle H, const unify::Image& Image)
{
    if (!Handles.valid(H)) return false;
    Textures[int32(H.index())] = MakeTexture(Image);
    Sizes[int32(H.index())] = FIntPoint(Image.width, Image.height);
    return true;
}

bool FUnifyUnrealRenderDevice::destroy_texture(unify::TextureHandle H)
{
    if (!Handles.valid(H)) return false;
    Textures[int32(H.index())] = nullptr;  // released to Unreal's GC
    return Handles.release(H);
}

bool FUnifyUnrealRenderDevice::texture_size(unify::TextureHandle H, int& W, int& Hh) const
{
    if (!Handles.valid(H)) return false;
    W = Sizes[int32(H.index())].X;
    Hh = Sizes[int32(H.index())].Y;
    return true;
}

void FUnifyUnrealRenderDevice::begin(unify::RenderTarget&, unify::Color Clear)
{
    Frame.Reset();
    ClearColor = FLinearColor(FColor(Clear.r, Clear.g, Clear.b, Clear.a));
    if (!White) White = MakeTexture(unify::Image(1, 1, {255, 255, 255, 255}));
}

void FUnifyUnrealRenderDevice::draw(const unify::Batch& Batch)
{
    FRecordedBatch& Out = Frame.AddDefaulted_GetRef();
    UTexture2D* Tex = White;
    FIntPoint Size(1, 1);
    unify::TextureHandle H{Batch.texture};
    if (Batch.texture && Handles.valid(H)) { Tex = Textures[int32(H.index())]; Size = Sizes[int32(H.index())]; }
    Out.Texture = Tex;
    Out.Scissor = Batch.scissor;
    switch (Batch.material.blend)
    {
        case unify::BlendMode::Opaque: Out.Blend = SE_BLEND_Opaque; break;
        case unify::BlendMode::Additive: Out.Blend = SE_BLEND_Additive; break;
        case unify::BlendMode::Multiply: Out.Blend = SE_BLEND_Modulate; break;
        default: Out.Blend = SE_BLEND_Translucent; break;
    }
    Out.Tris.Reserve(Batch.quad_count * 2);
    for (uint32 Q = 0; Q < Batch.quad_count; ++Q)
    {
        const unify::Vertex* V = Batch.vertices + Q * 4;
        auto Vert = [&](const unify::Vertex& X, FVector2D& Pos, FVector2D& UV, FLinearColor& Col)
        {
            Pos = FVector2D(X.x, X.y);
            UV = FVector2D(X.u / Size.X, X.v / Size.Y);  // UNIFY UVs are texels; Unreal wants 0..1
            Col = FLinearColor(FColor(X.color.r, X.color.g, X.color.b, X.color.a));
        };
        static const int32 QuadTris[2][3] = {{0, 1, 2}, {0, 2, 3}};
        for (const int32* Tri : QuadTris)
        {
            FCanvasUVTri T;
            Vert(V[Tri[0]], T.V0_Pos, T.V0_UV, T.V0_Color);
            Vert(V[Tri[1]], T.V1_Pos, T.V1_UV, T.V1_Color);
            Vert(V[Tri[2]], T.V2_Pos, T.V2_UV, T.V2_Color);
            Out.Tris.Add(T);
        }
    }
}

void FUnifyUnrealRenderDevice::FlushToCanvas(UCanvas* Canvas, float LogicalWidth, float LogicalHeight)
{
    if (!Canvas) return;
    const float Scale = FMath::Max(1.0f, FMath::FloorToFloat(FMath::Min(Canvas->ClipX / LogicalWidth, Canvas->ClipY / LogicalHeight)));
    const FVector2D Offset((Canvas->ClipX - LogicalWidth * Scale) * 0.5f, (Canvas->ClipY - LogicalHeight * Scale) * 0.5f);
    FCanvasTileItem Background(Offset, FVector2D(LogicalWidth * Scale, LogicalHeight * Scale), ClearColor);
    Canvas->DrawItem(Background);
    for (FRecordedBatch& B : Frame)
    {
        TArray<FCanvasUVTri> Scaled = B.Tris;
        for (FCanvasUVTri& T : Scaled)
        {
            T.V0_Pos = Offset + T.V0_Pos * Scale;
            T.V1_Pos = Offset + T.V1_Pos * Scale;
            T.V2_Pos = Offset + T.V2_Pos * Scale;
        }
        // Scissor rectangles are recorded (B.Scissor) but not yet applied on this backend.
        FCanvasTriangleItem Item(Scaled, B.Texture->GetResource());
        Item.BlendMode = B.Blend;
        Canvas->DrawItem(Item);
    }
}

void FUnifyUnrealRenderDevice::AddReferencedObjects(FReferenceCollector& Collector)
{
    Collector.AddReferencedObjects(Textures);
    Collector.AddReferencedObject(White);
}

// ---------------------------------------------------------------------------------- audio

bool UUnifySynthComponent::Init(int32& SampleRate)
{
    NumChannels = 2;
    SampleRate = Mixer ? int32(Mixer->device_rate()) : 44100;
    return true;
}

int32 UUnifySynthComponent::OnGenerateAudio(float* OutAudio, int32 NumSamples)
{
    // Audio render thread: UNIFY's mixer is designed to be driven from exactly this kind of thread.
    if (Mixer) Mixer->render(OutAudio, uint32(NumSamples / 2));
    else FMemory::Memzero(OutAudio, NumSamples * sizeof(float));
    return NumSamples;
}

bool FUnifyUnrealAudioDevice::start(unify::Mixer& Mixer)
{
    if (!Synth) return false;
    Synth->Mixer = &Mixer;
    Synth->Start();
    return true;
}

void FUnifyUnrealAudioDevice::stop()
{
    if (Synth) { Synth->Stop(); Synth->Mixer = nullptr; }
}

// ---------------------------------------------------------------------------------- backend

bool FUnifyUnrealBackend::init(const unify::EngineConfig&, std::string& Error)
{
    if (!Synth) { Error = "UUnifySynthComponent missing (AUnifyHost creates it)"; return false; }
    return true;
}

uint64_t FUnifyUnrealBackend::now_ns()
{
    return uint64_t(double(FPlatformTime::Cycles64()) * FPlatformTime::GetSecondsPerCycle64() * 1e9);
}

static uint16 MapKey(const FKey& Key)
{
    using namespace unify;
    if (Key == EKeys::SpaceBar) return key::Space;
    if (Key == EKeys::Left) return key::Left;
    if (Key == EKeys::Right) return key::Right;
    if (Key == EKeys::Up) return key::Up;
    if (Key == EKeys::Down) return key::Down;
    if (Key == EKeys::Enter) return key::Enter;
    if (Key == EKeys::Escape) return key::Escape;
    if (Key == EKeys::F5) return key::F5;
    if (Key == EKeys::F9) return key::F9;
    if (Key == EKeys::A) return key::A;
    if (Key == EKeys::D) return key::D;
    if (Key == EKeys::W) return key::W;
    if (Key == EKeys::S) return key::S;
    if (Key == EKeys::LeftMouseButton) return key::MouseLeft;
    if (Key == EKeys::Gamepad_FaceButton_Bottom) return key::PadA;
    if (Key == EKeys::Gamepad_FaceButton_Right) return key::PadB;
    if (Key == EKeys::Gamepad_Special_Right) return key::PadStart;
    if (Key == EKeys::Gamepad_DPad_Left) return key::PadLeft;
    if (Key == EKeys::Gamepad_DPad_Right) return key::PadRight;
    if (Key == EKeys::Gamepad_LeftX) return key::PadAxisLeftX;
    if (Key == EKeys::Gamepad_LeftY) return key::PadAxisLeftY;
    return 0;
}

void FUnifyUnrealBackend::QueueKey(const FKey& Key, bool bDown, bool bRepeat)
{
    const uint16 Code = MapKey(Key);
    if (!Code) return;
    unify::InputEvent E;
    E.timestamp_ns = now_ns();
    E.device = Key.IsGamepadKey() ? unify::InputDevice::Gamepad : Key.IsMouseButton() ? unify::InputDevice::Mouse : unify::InputDevice::Keyboard;
    E.type = !bDown ? unify::InputEventType::ButtonUp : bRepeat ? unify::InputEventType::ButtonRepeat : unify::InputEventType::ButtonDown;
    E.code = Code;
    FScopeLock Lock(&PendingLock);
    Pending.Add(E);
}

void FUnifyUnrealBackend::QueueAxis(const FKey& Key, float Value)
{
    const uint16 Code = MapKey(Key);
    if (!Code) return;
    unify::InputEvent E;
    E.timestamp_ns = now_ns();
    E.device = unify::InputDevice::Gamepad;
    E.type = unify::InputEventType::Axis;
    E.code = Code;
    E.value = Value;
    FScopeLock Lock(&PendingLock);
    Pending.Add(E);
}

void FUnifyUnrealBackend::QueuePointer(float X, float Y)
{
    unify::InputEvent E;
    E.timestamp_ns = now_ns();
    E.device = unify::InputDevice::Mouse;
    E.type = unify::InputEventType::PointerMove;
    E.value = X;
    E.value2 = Y;
    FScopeLock Lock(&PendingLock);
    Pending.Add(E);
}

void FUnifyUnrealBackend::poll(unify::InputSystem& Input, bool& Quit)
{
    TArray<unify::InputEvent> Events;
    {
        FScopeLock Lock(&PendingLock);
        Events = MoveTemp(Pending);
    }
    for (const unify::InputEvent& E : Events) Input.push(E);
    Quit = Quit || bQuitRequested;
}

void FUnifyUnrealBackend::present(const unify::RenderTarget&)
{
    // Batches were recorded by FUnifyUnrealRenderDevice::draw(); AUnifyHUD::DrawHUD flushes
    // them onto the canvas in Unreal's own draw pass.
}

void FUnifyUnrealBackend::shutdown()
{
    Audio.stop();
}
