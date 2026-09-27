// Hosts a unify::Engine inside an Unreal level. Drop AUnifyHost into a map (or spawn it from a
// GameMode) and set AUnifyHUD as the HUD class. STATUS: see UnifyUnrealBackend.h.
#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/HUD.h"
#include <memory>
#include "UnifyHost.generated.h"

class FUnifyUnrealBackend;
namespace unify { class Engine; }

UCLASS()
class UNIFYUNREAL_API AUnifyHost : public AActor
{
    GENERATED_BODY()
public:
    AUnifyHost();
    virtual void BeginPlay() override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    virtual void Tick(float DeltaSeconds) override;
    static void AddReferencedObjects(UObject* This, FReferenceCollector& Collector);

    /// UNIFY asset root (scenes/, prefabs/, textures/ ...), relative to the project directory.
    UPROPERTY(EditAnywhere, Category = "UNIFY") FString AssetRoot = TEXT("Unify/assets");
    UPROPERTY(EditAnywhere, Category = "UNIFY") FString BootScene = TEXT("title");

    void DrawToCanvas(UCanvas* Canvas);

private:
    void HandleKeyDown(FKey Key);
    void HandleKeyUp(FKey Key);

    UPROPERTY() TObjectPtr<class UUnifySynthComponent> Synth;
    std::unique_ptr<FUnifyUnrealBackend> Backend;
    std::unique_ptr<unify::Engine> Engine;
};

UCLASS()
class UNIFYUNREAL_API AUnifyHUD : public AHUD
{
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
};
