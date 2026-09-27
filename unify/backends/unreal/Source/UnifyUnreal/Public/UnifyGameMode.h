#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "UnifyGameMode.generated.h"
UCLASS()
class UNIFYUNREAL_API AUnifyGameMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AUnifyGameMode();
    virtual void StartPlay() override;
};
