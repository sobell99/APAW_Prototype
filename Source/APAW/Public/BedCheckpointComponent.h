/*
 * Bed Checkpoint Save System
 * Header file 
 *
 * B.L
 * November 24, 2026
 */
#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Templates/SubclassOf.h"
#include "TimerManager.h"
#include "BedCheckpointComponent.generated.h"

class ACharacter;
class UBoxComponent;

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class APAW_API UBedCheckpointComponent : public UActorComponent
{
    GENERATED_BODY()

public:
    UBedCheckpointComponent();

    UFUNCTION(BlueprintCallable, Category = "Checkpoint")
    bool SaveCheckpoint(UBoxComponent* BedBox);

    UPROPERTY(EditAnywhere, Category = "Checkpoint")
    TSubclassOf<ACharacter> CatClass;

    UPROPERTY(EditAnywhere, Category = "Checkpoint")
    TSubclassOf<ACharacter> DogClass;

    // enable this on the placed bed in the map
    UPROPERTY(EditInstanceOnly, Category = "Checkpoint")
    bool bRestoreOnBeginPlay = false;

protected:
    void BeginPlay() override;
    void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
    bool FindReadyCharacters(ACharacter*& Cat, ACharacter*& Dog) const;
    void TryRestoreCheckpoint();

    FTimerHandle RestoreTimer;
    int32 RestoreAttempts = 0;
};