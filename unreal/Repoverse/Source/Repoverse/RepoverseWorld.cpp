#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "RepoverseWorld.h"
#include "ProceduralMeshComponent.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/SceneComponent.h"
#include "DrawDebugHelpers.h"
#include "Components/DirectionalLightComponent.h"
#include "Engine/DirectionalLight.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HttpModule.h"
#include "Interfaces/IHttpResponse.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Materials/MaterialInstanceDynamic.h"

namespace {
ARepoverseWorld* WorldBridge(UWorld* W) {
    for(TActorIterator<ARepoverseWorld> I(W);I;++I) return *I;
    return nullptr;
}
FString Encode(const TSharedRef<FJsonObject>& O) {
    FString S;FJsonSerializer::Serialize(O,TJsonWriterFactory<>::Create(&S));return S;
}
FVector Position(const TSharedPtr<FJsonObject>& O) {
    return RepoverseSpace::ToUnreal(O->GetNumberField(TEXT("x")),O->GetNumberField(TEXT("y")),O->GetNumberField(TEXT("z")));
}
}
ARepoverseWorld::ARepoverseWorld() {
    PrimaryActorTick.bCanEverTick=true;
    RootComponent=CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
}
void ARepoverseWorld::BeginPlay() {
    Super::BeginPlay();
    FParse::Value(FCommandLine::Get(),TEXT("RepoverseToken="),Token);
    FParse::Value(FCommandLine::Get(),TEXT("RepoversePort="),Port);
    WorldMaterial=LoadObject<UMaterialInterface>(nullptr,TEXT("/Game/Materials/M_Repoverse.M_Repoverse"));
    if(Token.Len()<32 || Port<1024 || Port>65535) {Status=TEXT("Launch with tools/Start-Repoverse3D.ps1 (missing local session token).");return;}
    Enqueue(TEXT("/v1/session"),TEXT("{}"),[this](TSharedPtr<FJsonObject> O) {
        if(O->GetIntegerField(TEXT("schemaVersion"))!=1){Status=TEXT("Unsupported bridge version");return;}
        for(const auto& V:O->GetArrayField(TEXT("destinations"))) {
            auto D=V->AsObject();FRepoDestination R;
            R.Repo=D->GetStringField(TEXT("repo"));R.Spawn=Position(D);
            R.Floors=D->GetIntegerField(TEXT("floors"));
            R.Ladder=RepoverseSpace::ToUnreal(D->GetNumberField(TEXT("ladderX"))+.5,9,D->GetNumberField(TEXT("ladderZ"))+.5);
            Destinations.Add(R);
        }
        for(const auto& V:O->GetArrayField(TEXT("objects"))) {
            auto D=V->AsObject();FRepoObject R;
            R.Repo=D->GetStringField(TEXT("repo"));R.Path=D->GetStringField(TEXT("sourcePath"));R.Position=Position(D)+FVector(25,25,25);
            R.Uri=TEXT("repoverse://")+R.Repo+TEXT("/")+R.Path;
            for(const auto& A:D->GetArrayField(TEXT("actions")))R.Actions.Add(A->AsString());
            Objects.Add(R);
        }
        ApplyBuilders(O);RestorePlayer(O->GetObjectField(TEXT("player")));bReady=true;Status=TEXT("Loading nearby district...");
    });
}
void ARepoverseWorld::Enqueue(FString Route,FString Body,TFunction<void(TSharedPtr<FJsonObject>)> Complete) {
    if(!bEnding && Commands.Num()<16)Commands.Add({MoveTemp(Route),MoveTemp(Body),MoveTemp(Complete)});
}
void ARepoverseWorld::StartNext() {
    if(bBusy || Commands.IsEmpty() || bEnding)return;
    FRepoCommand C=MoveTemp(Commands[0]);Commands.RemoveAt(0);bBusy=true;
    auto R=FHttpModule::Get().CreateRequest();ActiveRequest=R;
    R->SetURL(FString::Printf(TEXT("http://127.0.0.1:%d%s"),Port,*C.Route));
    R->SetVerb(C.Route.StartsWith(TEXT("/v1/source?"))?TEXT("GET"):TEXT("POST"));
    R->SetHeader(TEXT("X-Repoverse-Token"),Token);R->SetHeader(TEXT("Content-Type"),TEXT("application/json"));
    R->SetContentAsString(C.Body);R->SetTimeout(15);
    R->OnProcessRequestComplete().BindWeakLambda(this,[this,Complete=MoveTemp(C.Complete)](FHttpRequestPtr,FHttpResponsePtr Response,bool Ok) {
        bBusy=false;ActiveRequest.Reset();if(bEnding)return;
        TSharedPtr<FJsonObject> O;
        if(!Ok || !Response.IsValid()) {Status=TEXT("World service disconnected. Check launcher log; restart the game to reconnect.");bTerrainReady=false;return;}
        if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Response->GetContentAsString()),O) || !O.IsValid()) {Status=TEXT("Invalid world response");return;}
        if(Response->GetResponseCode()!=200) {FString Error;O->TryGetStringField(TEXT("error"),Error);Status=Error.IsEmpty()?TEXT("World request failed"):Error;return;}
        Complete(O);
    });
    if(!R->ProcessRequest()){bBusy=false;ActiveRequest.Reset();Status=TEXT("Could not contact world service");}
}
void ARepoverseWorld::Tick(float Delta) {
    Super::Tick(Delta);Poll+=Delta;
    if(bReady && !bBusy && Commands.IsEmpty() && Poll>.2f) {
        Poll=0;auto P=UGameplayStatics::GetPlayerPawn(this,0);
        if(P){auto V=RepoverseSpace::ToCore(P->GetActorLocation());auto O=MakeShared<FJsonObject>();
            O->SetNumberField(TEXT("x"),V.X);O->SetNumberField(TEXT("z"),V.Z);
            Enqueue(TEXT("/v1/focus"),Encode(O),[this](TSharedPtr<FJsonObject> D){
                for(const auto& U:D->GetArrayField(TEXT("unloaded"))) {TObjectPtr<UProceduralMeshComponent> M;if(Chunks.RemoveAndCopyValue(U->AsString(),M)&&M)M->DestroyComponent();}
                for(const auto& C:D->GetArrayField(TEXT("chunks")))ApplyMesh(C->AsObject());
                ApplyBuilders(D);Resident=D->GetIntegerField(TEXT("resident"));
                bTerrainReady=D->GetIntegerField(TEXT("remaining"))==0;
                if(bTerrainReady){if(Status==TEXT("Loading nearby district..."))Status=TEXT("District ready");}
            });
        }
    }
    auto Player=UGameplayStatics::GetPlayerPawn(this,0);
    if(Player)for(const auto& O:Objects)if(FVector::DistSquared(Player->GetActorLocation(),O.Position)<1000*1000)
        DrawDebugBox(GetWorld(),O.Position,FVector(12,12,20),FColor::Cyan,false,-1,0,2);
    StartNext();
}
void ARepoverseWorld::ApplyBuilders(const TSharedPtr<FJsonObject>& Packet) {
    const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;
    if(!Packet->TryGetArrayField(TEXT("builders"),Values))return;
    TSet<FString> Present;
    for(const auto& Value:*Values){
        auto B=Value->AsObject();FString Name=B->GetStringField(TEXT("actor"));Present.Add(Name);
        auto& Body=Builders.FindOrAdd(Name);
        if(!Body){
            Body=NewObject<UStaticMeshComponent>(this);Body->SetupAttachment(RootComponent);Body->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Cube.Cube")));
            Body->SetCollisionEnabled(ECollisionEnabled::NoCollision);Body->RegisterComponent();Body->SetWorldScale3D(FVector(.3,.25,.5));
            auto Head=NewObject<UStaticMeshComponent>(this);Head->SetupAttachment(Body);Head->SetStaticMesh(LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Sphere.Sphere")));
            Head->SetCollisionEnabled(ECollisionEnabled::NoCollision);Head->RegisterComponent();Head->SetRelativeLocation(FVector(0,0,85));Head->SetWorldScale3D(FVector(.28));
        }
        Body->SetWorldLocation(Position(B)+FVector(0,0,25));
        DrawDebugString(GetWorld(),Body->GetComponentLocation()+FVector(0,0,65),Name,nullptr,FColor::Cyan,.3f,false);
    }
    TArray<FString> Removed;for(const auto& Pair:Builders)if(!Present.Contains(Pair.Key))Removed.Add(Pair.Key);
    for(const auto& Name:Removed){auto Body=Builders.FindAndRemoveChecked(Name);if(Body){TArray<USceneComponent*> Children;Body->GetChildrenComponents(true,Children);for(auto C:Children)C->DestroyComponent();Body->DestroyComponent();}}
}
void ARepoverseWorld::ApplyMesh(const TSharedPtr<FJsonObject>& Packet) {
    const FString Id=Packet->GetStringField(TEXT("id"));
    auto& M=Chunks.FindOrAdd(Id);
    if(!M){M=NewObject<UProceduralMeshComponent>(this);M->SetupAttachment(RootComponent);M->RegisterComponent();M->bUseComplexAsSimpleCollision=true;M->SetCollisionProfileName(TEXT("BlockAll"));}
    M->ClearAllMeshSections();int32 Section=0;
    for(const auto& V:Packet->GetArrayField(TEXT("sections"))) {
        auto S=V->AsObject();TArray<FVector> Vertices,Normals;TArray<int32> Indices;TArray<FVector2D> UV;
        TArray<FLinearColor> Colors;TArray<FProcMeshTangent> Tangents;
        const int32 Key=S->GetIntegerField(TEXT("key"));
        const int32 Type=Key&255;
        FLinearColor Color=FLinearColor::MakeFromHSV8(uint8((Key>>8)*27),130,200);
        if(Type==3)Color=FLinearColor(.08,.22,.14);if(Type==4)Color=FLinearColor(.04,.05,.07);
        if(Type==10)Color=FLinearColor(.18,.65,.85);if(Type==9)Color*=.65f;
        for(const auto& P:S->GetArrayField(TEXT("vertices"))){const auto& A=P->AsArray();Vertices.Add(RepoverseSpace::ToUnreal(A[0]->AsNumber(),A[1]->AsNumber(),A[2]->AsNumber()));Colors.Add(Color);UV.Add(FVector2D::ZeroVector);Normals.Add(FVector::ZeroVector);}
        const auto& I=S->GetArrayField(TEXT("indices"));
        // Y/Z swap reverses handedness: flip each triangle exactly once.
        for(int32 N=0;N+2<I.Num();N+=3){Indices.Add(int32(I[N]->AsNumber()));Indices.Add(int32(I[N+2]->AsNumber()));Indices.Add(int32(I[N+1]->AsNumber()));}
        for(int32 N=0;N+2<Indices.Num();N+=3){int32 A=Indices[N],B=Indices[N+1],C=Indices[N+2];FVector Normal=FVector::CrossProduct(Vertices[B]-Vertices[A],Vertices[C]-Vertices[A]).GetSafeNormal();Normals[A]=Normals[B]=Normals[C]=Normal;}
        M->CreateMeshSection_LinearColor(Section,Vertices,Indices,Normals,UV,Colors,Tangents,S->GetBoolField(TEXT("collision")));
        if(WorldMaterial)M->SetMaterial(Section,WorldMaterial);Section++;
    }
}
void ARepoverseWorld::RestorePlayer(const TSharedPtr<FJsonObject>& O) {
    auto P=UGameplayStatics::GetPlayerPawn(this,0);if(!P)return;
    P->SetActorLocation(Position(O),false,nullptr,ETeleportType::TeleportPhysics);
    if(auto C=Cast<ACharacter>(P))C->GetCharacterMovement()->StopMovementImmediately();
    if(auto PC=UGameplayStatics::GetPlayerController(this,0))PC->SetControlRotation(FRotator(0,O->GetNumberField(TEXT("yaw")),0));
    Selection.Empty();O->TryGetStringField(TEXT("selection"),Selection);
    bCode=false;Mode=0;bTerrainReady=false;Status=TEXT("Loading nearby district...");
    // Restore inspection after the initial manifest objects have been received.
    if(O->GetStringField(TEXT("mode"))==TEXT("code"))for(int32 I=0;I<Objects.Num();I++)if(Objects[I].Uri==Selection){Inspected=I;bCode=true;ReadPage();break;}
}
void ARepoverseWorld::Save(bool bQuit) {
    if(!bReady)return;auto P=UGameplayStatics::GetPlayerPawn(this,0);if(!P)return;
    auto V=RepoverseSpace::ToCore(P->GetActorLocation());auto O=MakeShared<FJsonObject>();
    O->SetNumberField(TEXT("x"),V.X);O->SetNumberField(TEXT("y"),V.Y);O->SetNumberField(TEXT("z"),V.Z);
    O->SetNumberField(TEXT("yaw"),P->GetControlRotation().Yaw);O->SetStringField(TEXT("mode"),bCode?TEXT("code"):TEXT("world"));
    O->SetStringField(TEXT("selection"),Selection);
    Enqueue(TEXT("/v1/save"),Encode(O),[this,bQuit](TSharedPtr<FJsonObject>){Status=TEXT("World saved");if(bQuit)if(auto PC=UGameplayStatics::GetPlayerController(this,0))PC->ConsoleCommand(TEXT("quit"));});
}
void ARepoverseWorld::Load(){if(bReady)Enqueue(TEXT("/v1/load"),TEXT("{}"),[this](TSharedPtr<FJsonObject> O){RestorePlayer(O);});}
void ARepoverseWorld::Travel(int32 Step) {
    if(!bReady || Destinations.IsEmpty())return;
    CurrentDestination=(CurrentDestination+Step+Destinations.Num())%Destinations.Num();
    auto P=UGameplayStatics::GetPlayerPawn(this,0);if(!P)return;
    P->SetActorLocation(Destinations[CurrentDestination].Spawn,false,nullptr,ETeleportType::TeleportPhysics);
    if(auto C=Cast<ACharacter>(P))C->GetCharacterMovement()->StopMovementImmediately();
    Mode=0;bCode=false;bTerrainReady=false;Status=TEXT("Loading nearby district...");
}
bool ARepoverseWorld::OnLadder(const FVector& P) const {
    for(const auto& D:Destinations)if(FVector::DistSquared2D(P,D.Ladder)<40*40 && P.Z>=400 && P.Z<(9+D.Floors*6)*50)return true;
    return false;
}
void ARepoverseWorld::Inspect() {
    auto P=UGameplayStatics::GetPlayerPawn(this,0);if(!P)return;
    double Nearest=180*180;Inspected=INDEX_NONE;
    for(int32 I=0;I<Objects.Num();I++){double D=FVector::DistSquared(P->GetActorLocation(),Objects[I].Position);if(D<Nearest&&Objects[I].Actions.Contains(TEXT("inspect"))){Nearest=D;Inspected=I;}}
    if(Inspected==INDEX_NONE){Status=TEXT("Move near a glowing source terminal and press E");return;}
    bCode=true;SourceOffset=0;Selection=Objects[Inspected].Uri;ReadPage();
}
void ARepoverseWorld::Page(int32 Step){if(bCode){SourceOffset=FMath::Max(0,SourceOffset+Step*32);ReadPage();}}
void ARepoverseWorld::ReadPage() {
    if(!Objects.IsValidIndex(Inspected))return;const auto& O=Objects[Inspected];
    Source=TEXT("Loading source...");
    FString Route=FString::Printf(TEXT("/v1/source?repo=%s&path=%s&offset=%d"),*FGenericPlatformHttp::UrlEncode(O.Repo),*FGenericPlatformHttp::UrlEncode(O.Path),SourceOffset);
    Enqueue(Route,TEXT(""),[this](TSharedPtr<FJsonObject> P){Source.Empty();int32 N=P->GetIntegerField(TEXT("offset"))+1;for(const auto& L:P->GetArrayField(TEXT("lines")))Source+=FString::Printf(TEXT("%5d  %s\n"),N++,*L->AsString());});
}
void ARepoverseWorld::EndPlay(const EEndPlayReason::Type Reason) {
    bEnding=true;Commands.Empty();if(ActiveRequest){ActiveRequest->OnProcessRequestComplete().Unbind();ActiveRequest->CancelRequest();ActiveRequest.Reset();}
    Super::EndPlay(Reason);
}
ARepoverseExplorer::ARepoverseExplorer() {
    PrimaryActorTick.bCanEverTick=true;GetCapsuleComponent()->InitCapsuleSize(16,60);
    View=CreateDefaultSubobject<UCameraComponent>(TEXT("View"));View->SetupAttachment(RootComponent);View->SetRelativeLocation(FVector(0,0,42));
    GetCharacterMovement()->MaxStepHeight=52;GetCharacterMovement()->JumpZVelocity=300;GetCharacterMovement()->MaxWalkSpeed=240;
    GetCharacterMovement()->BrakingDecelerationWalking=1800;bUseControllerRotationYaw=true;
}
void ARepoverseExplorer::Tick(float Delta) {
    Super::Tick(Delta);auto PC=Cast<APlayerController>(GetController());auto W=WorldBridge(GetWorld());if(!PC||!W)return;
    float DX=0,DY=0;PC->GetInputMouseDelta(DX,DY);PC->AddYawInput(DX);PC->AddPitchInput(-DY);
    auto Down=[PC](FKey K){return PC->IsInputKeyDown(K);};auto Press=[PC](FKey K){return PC->WasInputKeyJustPressed(K);};
    if(Press(EKeys::F5))W->Save();if(Press(EKeys::F9))W->Load();
    if(Press(EKeys::Tab))W->Travel(Down(EKeys::LeftShift)?-1:1);
    if(Press(EKeys::E))W->Inspect();if(Press(EKeys::PageDown))W->Page(1);if(Press(EKeys::PageUp))W->Page(-1);
    if(Press(EKeys::Escape)){if(W->bCode)W->bCode=false;else if(W->bReady)W->Save(true);else PC->ConsoleCommand(TEXT("quit"));}
    if(Press(EKeys::One))W->Mode=0;if(Press(EKeys::Two))W->Mode=1;if(Press(EKeys::Three))W->Mode=2;
    const bool Frozen=!W->bTerrainReady||W->bCode||W->Mode!=0;
    if(Frozen){GetCharacterMovement()->StopMovementImmediately();GetCharacterMovement()->SetMovementMode(MOVE_None);}
    else {
        float Forward=(Down(EKeys::W)||Down(EKeys::Up)?1.f:0.f)-(Down(EKeys::S)||Down(EKeys::Down)?1.f:0.f);
        float Right=(Down(EKeys::D)||Down(EKeys::Right)?1.f:0.f)-(Down(EKeys::A)||Down(EKeys::Left)?1.f:0.f);
        bool Ladder=W->OnLadder(GetActorLocation())&&(Down(EKeys::SpaceBar)||Down(EKeys::LeftControl));
        if(Ladder)GetCharacterMovement()->SetMovementMode(MOVE_Flying);
        else if(GetCharacterMovement()->MovementMode==MOVE_None || GetCharacterMovement()->MovementMode==MOVE_Flying)GetCharacterMovement()->SetMovementMode(MOVE_Walking);
        const FRotator Yaw(0,PC->GetControlRotation().Yaw,0);
        AddMovementInput(Yaw.Vector(),Forward);AddMovementInput(FRotationMatrix(Yaw).GetUnitAxis(EAxis::Y),Right);
        if(Ladder)AddMovementInput(FVector::UpVector,Down(EKeys::SpaceBar)?1:-1);
        else if(Press(EKeys::SpaceBar))Jump();
        GetCharacterMovement()->MaxWalkSpeed=Down(EKeys::LeftShift)?400:240;
        if(GetActorLocation().Z<0)W->Travel(0);
    }
    if(W->Mode==0){View->SetWorldLocation(GetActorLocation()+FVector(0,0,42));View->SetWorldRotation(PC->GetControlRotation());}
    else {FVector Target=GetActorLocation();if(W->Destinations.IsValidIndex(W->CurrentDestination))Target=W->Destinations[W->CurrentDestination].Spawn-FVector(500,0,0);
        float Radius=W->Mode==1?1600:4200;FRotator Orbit(-35,PC->GetControlRotation().Yaw,0);FVector Eye=Target-Orbit.Vector()*Radius;View->SetWorldLocation(Eye);View->SetWorldRotation((Target-Eye).Rotation());}
}
void ARepoverseHUD::DrawHUD() {
    Super::DrawHUD();auto W=WorldBridge(GetWorld());if(!W||!Canvas)return;
    DrawRect(FLinearColor(0.015,0.025,0.05,.92),0,0,Canvas->SizeX,92);
    DrawText(TEXT("REPOVERSE / repository district"),FLinearColor(.3,.9,1),20,12,nullptr,1.3);
    DrawText(W->Status,FLinearColor::White,20,40);
    DrawText(TEXT("WASD walk | Mouse look | Space jump/climb | Ctrl descend | E inspect | Tab travel | 1 walk 2 orbit 3 overview | F5 save F9 load"),FLinearColor(.7,.8,.9),20,66);
    if(W->Destinations.IsValidIndex(W->CurrentDestination))DrawText(W->Destinations[W->CurrentDestination].Repo,FLinearColor(.8,.7,1),20,100);
    if(W->bCode){DrawRect(FLinearColor(.01,.02,.03,.97),12,130,Canvas->SizeX-24,Canvas->SizeY-142);
        DrawText(W->Selection+TEXT("  [PgUp/PgDn, Esc close]"),FLinearColor(.3,.9,1),22,140);
        DrawText(W->Source,FLinearColor(.9,.92,.95),22,164,nullptr,.9);
    } else {
        auto P=UGameplayStatics::GetPlayerPawn(this,0);
        for(const auto& O:W->Objects)if(P&&FVector::DistSquared(P->GetActorLocation(),O.Position)<400*400){FVector S=Project(O.Position);if(S.Z>0){DrawRect(FLinearColor(.2,.9,1,1),S.X-3,S.Y-3,6,6);if(FVector::DistSquared(P->GetActorLocation(),O.Position)<180*180)DrawText(O.Path+TEXT(" [E]"),FLinearColor::White,S.X+8,S.Y);}}
    }
}
ARepoverseWorldMode::ARepoverseWorldMode(){DefaultPawnClass=ARepoverseExplorer::StaticClass();HUDClass=ARepoverseHUD::StaticClass();}
void ARepoverseWorldMode::StartPlay() {
    Super::StartPlay();GetWorld()->SpawnActor<ARepoverseWorld>();
    auto Light=GetWorld()->SpawnActor<ADirectionalLight>(FVector::ZeroVector,FRotator(-55,-30,0));if(Light)Light->GetLightComponent()->SetIntensity(4);
    if(auto PC=UGameplayStatics::GetPlayerController(this,0)){PC->SetInputMode(FInputModeGameOnly());PC->bShowMouseCursor=false;}
}
