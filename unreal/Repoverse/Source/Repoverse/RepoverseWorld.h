#pragma once
#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "Interfaces/IHttpRequest.h"
#include "RepoverseWorld.generated.h"

class UProceduralMeshComponent;
class UCameraComponent;
class UMaterialInterface;
class FJsonObject;

// One conversion at the renderer boundary: core Y-up voxels to UE Z-up centimetres.
namespace RepoverseSpace {
    constexpr double Scale = 50.0;
    inline FVector ToUnreal(double X,double Y,double Z) {return FVector(X,Z,Y)*Scale;}
    inline FVector ToCore(const FVector& P) {return FVector(P.X,P.Z,P.Y)/Scale;}
}
struct FRepoDestination {
    FString Repo;
    FVector Spawn;
    FVector Ladder;
    int32 Floors=1;
};
struct FRepoObject {
    FString Repo,Path,Uri;
    FVector Position;
    TArray<FString> Actions;
};
struct FRepoCommand {
    FString Route,Body;
    TFunction<void(TSharedPtr<FJsonObject>)> Complete;
};

UCLASS()
class REPOVERSE_API ARepoverseWorld : public AActor {
    GENERATED_BODY()
public:
    ARepoverseWorld();
    virtual void BeginPlay() override;
    virtual void Tick(float Delta) override;
    virtual void EndPlay(const EEndPlayReason::Type Reason) override;
    void Save(bool bQuit=false);
    void Load();
    void Travel(int32 Step);
    void Inspect();
    void Page(int32 Step);
    bool OnLadder(const FVector& Position) const;
    FString Status=TEXT("Connecting to local world...");
    FString Source;
    FString Selection;
    bool bCode=false;
    bool bReady=false;
    bool bTerrainReady=false;
    int32 Mode=0;
    int32 Resident=0;
    int32 CurrentDestination=0;
    TArray<FRepoDestination> Destinations;
    TArray<FRepoObject> Objects;
private:
    UPROPERTY() TMap<FString,TObjectPtr<UProceduralMeshComponent>> Chunks;
    UPROPERTY() TObjectPtr<UMaterialInterface> WorldMaterial;
    TArray<FRepoCommand> Commands;
    TSharedPtr<IHttpRequest,ESPMode::ThreadSafe> ActiveRequest;
    FString Token;
    int32 Port=38473;
    float Poll=0;
    bool bBusy=false;
    bool bEnding=false;
    int32 SourceOffset=0;
    int32 Inspected=INDEX_NONE;
    void Enqueue(FString Route,FString Body,TFunction<void(TSharedPtr<FJsonObject>)> Complete);
    void StartNext();
    void ApplyMesh(const TSharedPtr<FJsonObject>& Packet);
    void RestorePlayer(const TSharedPtr<FJsonObject>& Player);
    void ReadPage();
};

UCLASS()
class REPOVERSE_API ARepoverseExplorer : public ACharacter {
    GENERATED_BODY()
public:
    ARepoverseExplorer();
    virtual void Tick(float Delta) override;
    UPROPERTY() TObjectPtr<UCameraComponent> View;
};
UCLASS()
class REPOVERSE_API ARepoverseHUD : public AHUD {
    GENERATED_BODY()
public:
    virtual void DrawHUD() override;
};
UCLASS()
class REPOVERSE_API ARepoverseWorldMode : public AGameModeBase {
    GENERATED_BODY()
public:
    ARepoverseWorldMode();
    virtual void StartPlay() override;
};
