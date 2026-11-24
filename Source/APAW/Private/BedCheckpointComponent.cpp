/* 
 * Bed Checkpoint Save System
 * Main implementation
 * 
 * This does not currently save inventory as of 11/24/26
 * Saves independent positions and checkpoint acquisition for both cat and dog.
 * 
 * BoxComponent.h is referenced to give it collision properties.
 * If you see BedCheckpointComponent.generated.h, this is the same file!
 * Don't worry that it is not actually in the components folder if you get a warning log.
 * Warning messages are added for your debugging convenience.
 * 
 * B.L
 * November 24, 2026 
 */
#include "BedCheckpointComponent.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Components/BoxComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "Kismet/GameplayStatics.h"

DEFINE_LOG_CATEGORY_STATIC(LogBedCheckpoint, Log, All);

namespace
{
    const FString CheckpointSlot = TEXT("bed_checkpoint_v1");
    constexpr int32 CheckpointUserIndex = 0;
    constexpr uint32 CheckpointMagic = 0x42454431;
    constexpr int32 CheckpointVersion = 1;

    // find one pawn for this role in current world.
    // second match makes the role ambiguous, so fail before picking either pawn
    ACharacter* FindUniqueReadyCharacter(UWorld* World, UClass* CharacterClass)
    {
        ACharacter* Match = nullptr;

        for (TActorIterator<ACharacter> It(World, CharacterClass); It; ++It)
        {
            ACharacter* Candidate = *It;
            if (!IsValid(Candidate))
            {
                continue;
            }

            if (Match)
            {
                return nullptr;
            }

            Match = Candidate;
        }

        return Match && Match->HasActorBegunPlay() ? Match : nullptr;
    }
}

UBedCheckpointComponent::UBedCheckpointComponent()
{
    // bed calls SaveCheckpoint on interaction, so don't use ticking
    PrimaryComponentTick.bCanEverTick = false;
}

bool UBedCheckpointComponent::FindReadyCharacters(
    ACharacter*& Cat, ACharacter*& Dog) const
{
    // leave both outputs empty until we have a complete and validated pair
    Cat = nullptr;
    Dog = nullptr;

    UWorld* const World = GetWorld();
    UClass* const CatType = CatClass.Get();
    UClass* const DogType = DogClass.Get();

    if (!World || !CatType || !DogType || CatType == DogType)
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Assign distinct Cat Class and Dog Class values on the checkpoint."));
        return false;
    }

    ACharacter* const FoundCat = FindUniqueReadyCharacter(World, CatType);
    if (!FoundCat)
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Expected exactly one ready Cat of class %s."), *GetNameSafe(CatType));
        return false;
    }

    ACharacter* const FoundDog = FindUniqueReadyCharacter(World, DogType);
    if (!FoundDog || FoundDog == FoundCat)
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Expected exactly one ready Dog of class %s, distinct from Cat."),
            *GetNameSafe(DogType));
        return false;
    }

    Cat = FoundCat;
    Dog = FoundDog;
    return true;
}

bool UBedCheckpointComponent::SaveCheckpoint(UBoxComponent* BedBox)
{
    // bp supplies the box, reject missing boxes
    if (!IsValid(BedBox) || BedBox->GetOwner() != GetOwner())
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Connect this bed's Box component to the Bed Box pin!"));
        return false;
    }

    // player 0 is local player 
    // current pawn must be in range, other pawn can be elsewhere
    APawn* const PlayerPawn = UGameplayStatics::GetPlayerPawn(this, 0);
    if (!IsValid(PlayerPawn) || !BedBox->IsOverlappingActor(PlayerPawn))
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Local player's pawn is outside the bed box or unavailable"));
        return false;
    }

    ACharacter* Cat = nullptr;
    ACharacter* Dog = nullptr;
    if (!FindReadyCharacters(Cat, Dog))
    {
        return false;
    }

    if (PlayerPawn != Cat && PlayerPawn != Dog)
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Pawn using the bed is neither the configured Cat or Dog."));
        return false;
    }
    // capture world & map transforms before file write 
    // Char1_Pos for cat, Char2_Pos for dog
    FString MapName = UGameplayStatics::GetCurrentLevelName(
        this, /*bRemovePrefixString=*/true);
    FTransform Char1_Pos = Cat->GetActorTransform();
    FTransform Char2_Pos = Dog->GetActorTransform();

    if (MapName.IsEmpty() || !Char1_Pos.IsValid() || !Char2_Pos.IsValid())
    {
        UE_LOG(LogBedCheckpoint, Error,
            TEXT("Checkpoint write skipped: invalid map name or pawn transform."));
        return false;
    }

    // write format sig & version first
    // restore code will read the map and transform in this order
    uint32 Magic = CheckpointMagic;
    int32 Version = CheckpointVersion;
    TArray<uint8> SaveBytes;
    FMemoryWriter Writer(SaveBytes, /*bIsPersistent=*/true);
    Writer << Magic << Version << MapName << Char1_Pos << Char2_Pos;

    if (Writer.IsError()
        || !UGameplayStatics::SaveDataToSlot(
            SaveBytes, CheckpointSlot, CheckpointUserIndex))
    {
        UE_LOG(LogBedCheckpoint, Error, TEXT("Checkpoint write failed."));
        return false;
    }

    // return true after successful disk write
    UE_LOG(LogBedCheckpoint, Display, TEXT("Saved Cat %s | Dog %s"),
        *Char1_Pos.GetLocation().ToString(),
        *Char2_Pos.GetLocation().ToString());

    return true;
}
namespace
{
    constexpr float RestoreIntervalSeconds = 0.25f;
    constexpr int32 MaxRestoreAttempts = 40;
    constexpr int32 MaxCheckpointBytes = 4096;

    bool CanRestoreAt(const ACharacter& Character, const FTransform& Destination)
    {
        // clearance check uses pawn's curr collision size
        if (!Character.GetActorScale3D().Equals(Destination.GetScale3D()))
        {
            return false;
        }

        UWorld* const World = Character.GetWorld();
        if (!World)
        {
            return false;
        }

        const FVector SavedLocation = Destination.GetLocation();
        FVector CheckedLocation = SavedLocation;

        // let ue5 suggest a nearby position, accept saved spot only if adjustment is within 0.1 cm.
        return World->FindTeleportSpot(
            &Character, CheckedLocation, Destination.GetRotation().Rotator())
            && CheckedLocation.Equals(SavedLocation, 0.1);
    }

    bool ApplyCheckpointTransform(ACharacter* Character, const FTransform& Transform)
    {
        if (!IsValid(Character))
        {
            return false;
        }

        // pawn already at its saved transform counts as restored
        return Character->GetActorTransform().Equals(Transform)
            || Character->SetActorTransform(
                Transform, /*bSweep=*/false, nullptr, ETeleportType::TeleportPhysics);
    }
}

void UBedCheckpointComponent::BeginPlay()
{
    Super::BeginPlay();

    UWorld* const World = GetWorld();
    if (!bRestoreOnBeginPlay || !World || World->GetNetMode() == NM_Client
        || !UGameplayStatics::DoesSaveGameExist(CheckpointSlot, CheckpointUserIndex))
    {
        return;
    }

    // bed can start before the playable pawns
    // checks for both pawns every 0.25 sec for ~10 seconds
    RestoreAttempts = 0;
    World->GetTimerManager().SetTimer(
        RestoreTimer, this,
        &UBedCheckpointComponent::TryRestoreCheckpoint,
        RestoreIntervalSeconds, /*bLoop=*/true);
}

void UBedCheckpointComponent::TryRestoreCheckpoint()
{
    UWorld* const World = GetWorld();
    if (!World)
    {
        return;
    }

    ACharacter* Cat = nullptr;
    ACharacter* Dog = nullptr;
    if (!FindReadyCharacters(Cat, Dog))
    {
        if (++RestoreAttempts >= MaxRestoreAttempts)
        {
            World->GetTimerManager().ClearTimer(RestoreTimer);
            UE_LOG(LogBedCheckpoint, Warning,
                TEXT("Checkpoint restore timed out waiting for Cat and Dog."));
        }
        return;
    }

    // when both pawns ready, stop retrying, read slot x1 
    World->GetTimerManager().ClearTimer(RestoreTimer);

    TArray<uint8> SaveBytes;
    if (!UGameplayStatics::LoadDataFromSlot(
        SaveBytes, CheckpointSlot, CheckpointUserIndex)
        || SaveBytes.Num() > MaxCheckpointBytes)
    {
        UE_LOG(LogBedCheckpoint, Warning, TEXT("Could not read the bed checkpoint."));
        return;
    }

    // build same header SaveCheckpoint writes for map
    // also, i'm comparing the bytes to avoid decoding a damaged map-name field
    FString MapName = UGameplayStatics::GetCurrentLevelName(
        this, /*bRemovePrefixString=*/true);
    uint32 Magic = CheckpointMagic;
    int32 Version = CheckpointVersion;
    TArray<uint8> ExpectedHeader;
    FMemoryWriter HeaderWriter(ExpectedHeader, /*bIsPersistent=*/true);
    HeaderWriter << Magic << Version << MapName;

    if (SaveBytes.Num() <= ExpectedHeader.Num()
        || FMemory::Memcmp(SaveBytes.GetData(), ExpectedHeader.GetData(),
            ExpectedHeader.Num()) != 0)
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Checkpoint skipped: the file format or map does not match."));
        return;
    }

    // writer stores Cat first 
    FTransform SavedCat = FTransform::Identity;
    FTransform SavedDog = FTransform::Identity;
    FMemoryReader Reader(SaveBytes, /*bIsPersistent=*/true);
    Reader.Seek(ExpectedHeader.Num());
    Reader << SavedCat << SavedDog;

    if (Reader.IsError() || Reader.Tell() != SaveBytes.Num()
        || !SavedCat.IsValid() || !SavedDog.IsValid())
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Checkpoint skipped: the saved transforms are invalid."));
        return;
    }

    // check both destinations before moving either pawn
    if (!CanRestoreAt(*Cat, SavedCat) || !CanRestoreAt(*Dog, SavedDog))
    {
        UE_LOG(LogBedCheckpoint, Warning,
            TEXT("Checkpoint skipped: a saved spot is blocked or a pawn's scale changed."));
        return;
    }

    const FTransform OriginalCat = Cat->GetActorTransform();
    const FTransform OriginalDog = Dog->GetActorTransform();

    // check dog destination again, in case moving cat blocks it
    const bool bMovedBoth = ApplyCheckpointTransform(Cat, SavedCat)
        && IsValid(Dog)
        && CanRestoreAt(*Dog, SavedDog)
        && ApplyCheckpointTransform(Dog, SavedDog);

    const bool bRestoredBoth = bMovedBoth && IsValid(Cat) && IsValid(Dog)
        && Cat->GetActorTransform().Equals(SavedCat)
        && Dog->GetActorTransform().Equals(SavedDog);

    if (!bRestoredBoth)
    {
        const bool bCatRolledBack = ApplyCheckpointTransform(Cat, OriginalCat);
        const bool bDogRolledBack = ApplyCheckpointTransform(Dog, OriginalDog);

        UE_LOG(LogBedCheckpoint, Error,
            TEXT("Checkpoint restore failed. Rollback Cat=%d Dog=%d."),
            bCatRolledBack, bDogRolledBack);
        return;
    }

    // log actual landing position of both pawns
    UE_LOG(LogBedCheckpoint, Display, TEXT("Restored Cat %s | Dog %s"),
        *Cat->GetActorLocation().ToString(),
        *Dog->GetActorLocation().ToString());
}

void UBedCheckpointComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
    if (UWorld* const World = GetWorld())
    {
        World->GetTimerManager().ClearTimer(RestoreTimer);
    }

    Super::EndPlay(EndPlayReason);
}