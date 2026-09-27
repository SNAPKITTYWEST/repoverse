// STATUS: written against the UE5 API; not compiled in UNIFY's CI. See UnifyUnrealBackend.h.
#include "UnifyHost.h"
#include "UnifyUnrealBackend.h"
#include "EngineUtils.h"
#include "Components/InputComponent.h"
#include "GameFramework/PlayerController.h"
#include "Misc/Paths.h"
#include "runtime/engine.h"

AUnifyHost::AUnifyHost()
{
    PrimaryActorTick.bCanEverTick = true;
    Synth = CreateDefaultSubobject<UUnifySynthComponent>(TEXT("UnifyAudio"));
    RootComponent = Synth;
}

void AUnifyHost::BeginPlay()
{
    Super::BeginPlay();
    Backend = std::make_unique<FUnifyUnrealBackend>();
    Backend->Synth = Synth;

    unify::EngineConfig Config;
    Config.asset_root = TCHAR_TO_UTF8(*FPaths::Combine(FPaths::ProjectDir(), AssetRoot));
    Config.boot_scene = TCHAR_TO_UTF8(*BootScene);
    Engine = std::make_unique<unify::Engine>(*Backend, Config);
    std::string Error;
    if (!Engine->boot(Error))
    {
        UE_LOG(LogTemp, Error, TEXT("UNIFY boot failed: %s"), UTF8_TO_TCHAR(Error.c_str()));
        Engine.reset();
        return;
    }

    // Route every key through UNIFY's input buffer. UNIFY, not Unreal, decides what a key means.
    if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
    {
        EnableInput(PC);
        FInputKeyBinding Down(FInputChord(EKeys::AnyKey), IE_Pressed);
        Down.KeyDelegate.GetDelegateWithKeyForManualSet().BindUObject(this, &AUnifyHost::HandleKeyDown);
        InputComponent->KeyBindings.Add(Down);
        FInputKeyBinding Up(FInputChord(EKeys::AnyKey), IE_Released);
        Up.KeyDelegate.GetDelegateWithKeyForManualSet().BindUObject(this, &AUnifyHost::HandleKeyUp);
        InputComponent->KeyBindings.Add(Up);
    }
}

void AUnifyHost::HandleKeyDown(FKey Key) { if (Backend) Backend->QueueKey(Key, true, false); }
void AUnifyHost::HandleKeyUp(FKey Key) { if (Backend) Backend->QueueKey(Key, false, false); }

void AUnifyHost::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);
    // One UNIFY frame per Unreal frame: UNIFY runs its own fixed-step simulation inside it,
    // independent of Unreal's variable DeltaSeconds.
    if (Engine && !Engine->quit_requested()) Engine->frame();
}

void AUnifyHost::DrawToCanvas(UCanvas* Canvas)
{
    if (Engine) Backend->Device.FlushToCanvas(Canvas, float(Engine->config().width), float(Engine->config().height));
}

void AUnifyHost::EndPlay(const EEndPlayReason::Type Reason)
{
    Engine.reset();
    if (Backend) Backend->shutdown();
    Backend.reset();
    Super::EndPlay(Reason);
}

void AUnifyHost::AddReferencedObjects(UObject* This, FReferenceCollector& Collector)
{
    AUnifyHost* Self = CastChecked<AUnifyHost>(This);
    if (Self->Backend) Self->Backend->Device.AddReferencedObjects(Collector);
    Super::AddReferencedObjects(This, Collector);
}

void AUnifyHUD::DrawHUD()
{
    Super::DrawHUD();
    for (TActorIterator<AUnifyHost> It(GetWorld()); It; ++It) It->DrawToCanvas(Canvas);
}
