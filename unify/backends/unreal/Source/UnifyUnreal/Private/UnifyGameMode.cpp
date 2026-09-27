#include "UnifyGameMode.h"
#include "UnifyHost.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
AUnifyGameMode::AUnifyGameMode()
{
    HUDClass = AUnifyHUD::StaticClass();
    DefaultPawnClass = nullptr;
}
void AUnifyGameMode::StartPlay()
{
    Super::StartPlay();
    bool Found = false;
    for (TActorIterator<AUnifyHost> It(GetWorld()); It; ++It) { Found = true; break; }
    if (!Found) GetWorld()->SpawnActor<AUnifyHost>();
    if (APlayerController* PC = GetWorld()->GetFirstPlayerController())
    {
        PC->SetInputMode(FInputModeGameOnly());
        PC->bShowMouseCursor = false;
    }
}
