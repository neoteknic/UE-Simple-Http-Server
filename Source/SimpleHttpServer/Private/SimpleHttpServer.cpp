// Copyright (C) 2024 Mikhail Davydov (kaboms) - All Rights Reserved

#include "SimpleHttpServer.h"
#include "HttpPath.h"
#include "IHttpRouter.h"
#include "HttpServerHttpVersion.h"
#include "HttpServerModule.h"
#include "HttpServerResponse.h"
#include "Misc/EngineVersionComparison.h"

DEFINE_LOG_CATEGORY(LogSimpleHttpServer);

namespace
{
	FHttpPath MakeHttpPathForRoute(const FString& HttpPath)
	{
		return FHttpPath(HttpPath);
	}

	FString NormalizeHttpPath(FString InPath)
	{
		InPath.TrimStartAndEndInline();
		if (InPath.IsEmpty())
		{
			return TEXT("/");
		}

		if (!InPath.StartsWith(TEXT("/")))
		{
			InPath = TEXT("/") + InPath;
		}

		while (InPath.Len() > 1 && InPath.EndsWith(TEXT("/")))
		{
			InPath.LeftChopInline(1, false);
		}

		return InPath;
	}

	bool VerbsMatch(ENativeHttpServerRequestVerbs AllowedVerbs, EHttpServerRequestVerbs RequestVerb)
	{
		const uint8 AllowedMask = static_cast<uint8>(AllowedVerbs);
		const uint8 RequestMask = static_cast<uint8>((ENativeHttpServerRequestVerbs)RequestVerb);
		return (AllowedMask & RequestMask) != 0;
	}
}

void USimpleHttpServer::BeginDestroy()
{
	Super::BeginDestroy();

	StopServer();
}

void USimpleHttpServer::StartServer(int32 ServerPort)
{
	if (ServerPort <= 0)
	{
		UE_LOG(LogSimpleHttpServer, Error, TEXT("Could not start HttpServer, port number must be greater than zero!"));
		return;
	}

	CurrentServerPort = ServerPort;

	FHttpServerModule& HttpServerModule = FHttpServerModule::Get();

	HttpRouter = HttpServerModule.GetHttpRouter(CurrentServerPort);

	if (HttpRouter.IsValid())
	{
		BindRoutes();

		HttpServerModule.StartAllListeners();

		bServerStarted = true;
		UE_LOG(LogSimpleHttpServer, Log, TEXT("Web server started on port = %d"), CurrentServerPort);
	}
	else
	{
		bServerStarted = false;
		UE_LOG(LogSimpleHttpServer, Error, TEXT("Could not start web server on port = %d"), CurrentServerPort);
	}
}

void USimpleHttpServer::StopServer()
{
	UE_LOG(LogSimpleHttpServer, Log, TEXT("StopServer on Port: %d"), CurrentServerPort);

	FHttpServerModule& httpServerModule = FHttpServerModule::Get();
	httpServerModule.StopAllListeners();

	if (HttpRouter.IsValid())
	{
		if (bRootPreprocessorRegistered)
		{
			HttpRouter->UnregisterRequestPreprocessor(RootRequestPreprocessorHandle);
			bRootPreprocessorRegistered = false;
		}

		// Editor will crash after receive request if you start game from editor, close it and start again.
		// It is because HttpRouter lived in FHttpServerModule and don't be destroyed on game ending.
		// When server stopped or being destroyed we must unbind all handlers to prevent errors on the next game start.
		for (FHttpRouteHandle HttpRouteHandle : CreatedRouteHandlers)
		{
			HttpRouter->UnbindRoute(HttpRouteHandle);
		}
	}
}

void USimpleHttpServer::BindRoute(FString HttpPath, ENativeHttpServerRequestVerbs Verbs, FHttpServerRequestDelegate OnHttpServerRequest)
{
	const FString NormalizedPath = NormalizeHttpPath(HttpPath);
	RouteDelegates.Add(NormalizedPath, OnHttpServerRequest);
	if (ENativeHttpServerRequestVerbs* ExistingVerbs = RouteVerbs.Find(NormalizedPath))
	{
		*ExistingVerbs = (ENativeHttpServerRequestVerbs)((uint8)(*ExistingVerbs) | (uint8)Verbs);
	}
	else
	{
		RouteVerbs.Add(NormalizedPath, Verbs);
	}

	if (HttpRouter.IsValid())
	{
		if (NormalizedPath == TEXT("/"))
		{
			if (!bRootPreprocessorRegistered)
			{
				RootRequestPreprocessorHandle = HttpRouter->RegisterRequestPreprocessor(
					FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
					{
						if (!Request.RelativePath.IsRoot())
						{
							return false;
						}

						if (const ENativeHttpServerRequestVerbs* AllowedVerbs = RouteVerbs.Find(TEXT("/")))
						{
							if (!VerbsMatch(*AllowedVerbs, Request.Verb))
							{
								return false;
							}
						}

						if (RouteDelegates.Contains(TEXT("/")))
						{
							return HandleRequest(TEXT("/"), Request, OnComplete);
						}

						if (RouteHandlers.Contains(TEXT("/")))
						{
							return HandleRequestNative(TEXT("/"), Request, OnComplete);
						}

						return false;
					}));

				bRootPreprocessorRegistered = true;
			}

			return;
		}

		const FHttpPath RoutePath = MakeHttpPathForRoute(NormalizedPath);
		if (!RoutePath.IsValidPath())
		{
			UE_LOG(LogSimpleHttpServer, Error, TEXT("Invalid route path: '%s'. This route will not be bound."), *NormalizedPath);
			return;
		}

		FHttpRouteHandle HttpRouteHandle = HttpRouter->BindRoute(RoutePath, (EHttpServerRequestVerbs)Verbs,

		FHttpRequestHandler::CreateLambda([&, NormalizedPath](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleRequest(NormalizedPath, Request, OnComplete);
		}));


		CreatedRouteHandlers.Add(HttpRouteHandle);
	}
	else
	{
		UE_LOG(LogSimpleHttpServer, Error, TEXT("Failed bind to HttpRouter: router is invalid"));
	}
}

void USimpleHttpServer::BindRouteNative(FString HttpPath, ENativeHttpServerRequestVerbs Verbs, FHttpRouteHandler Handler)
{
	const FString NormalizedPath = NormalizeHttpPath(HttpPath);
	RouteHandlers.Add(NormalizedPath, Handler);
	if (ENativeHttpServerRequestVerbs* ExistingVerbs = RouteVerbs.Find(NormalizedPath))
	{
		*ExistingVerbs = (ENativeHttpServerRequestVerbs)((uint8)(*ExistingVerbs) | (uint8)Verbs);
	}
	else
	{
		RouteVerbs.Add(NormalizedPath, Verbs);
	}

	if (HttpRouter.IsValid())
	{
		if (NormalizedPath == TEXT("/"))
		{
			if (!bRootPreprocessorRegistered)
			{
				RootRequestPreprocessorHandle = HttpRouter->RegisterRequestPreprocessor(
					FHttpRequestHandler::CreateLambda([this](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
					{
						if (!Request.RelativePath.IsRoot())
						{
							return false;
						}

						if (const ENativeHttpServerRequestVerbs* AllowedVerbs = RouteVerbs.Find(TEXT("/")))
						{
							if (!VerbsMatch(*AllowedVerbs, Request.Verb))
							{
								return false;
							}
						}

						if (RouteDelegates.Contains(TEXT("/")))
						{
							return HandleRequest(TEXT("/"), Request, OnComplete);
						}

						if (RouteHandlers.Contains(TEXT("/")))
						{
							return HandleRequestNative(TEXT("/"), Request, OnComplete);
						}

						return false;
					}));

				bRootPreprocessorRegistered = true;
			}

			return;
		}

		const FHttpPath RoutePath = MakeHttpPathForRoute(NormalizedPath);
		if (!RoutePath.IsValidPath())
		{
			UE_LOG(LogSimpleHttpServer, Error, TEXT("Invalid route path: '%s'. This route will not be bound."), *NormalizedPath);
			return;
		}

		FHttpRouteHandle HttpRouteHandle = HttpRouter->BindRoute(RoutePath, (EHttpServerRequestVerbs)Verbs,

		FHttpRequestHandler::CreateLambda([&, NormalizedPath](const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
		{
			return HandleRequestNative(NormalizedPath, Request, OnComplete);
		}));


		CreatedRouteHandlers.Add(HttpRouteHandle);
	}
	else
	{
		UE_LOG(LogSimpleHttpServer, Error, TEXT("Failed bind to HttpRouter: router is invalid"));
	}
}

bool USimpleHttpServer::HandleRequest(FString HttpPath, const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FNativeHttpServerRequest NativeHttpServerRequest;
	FillNativeRequst(Request, NativeHttpServerRequest);

	if (FHttpServerRequestDelegate* HttpServerRequestDelegate = RouteDelegates.Find(HttpPath))
	{
		if ((*HttpServerRequestDelegate).IsBound())
		{
			FNativeHttpServerResponse HttpServerResponse = (*HttpServerRequestDelegate).Execute(NativeHttpServerRequest);
			TUniquePtr<FHttpServerResponse> Response = MakeUnique<FHttpServerResponse>();
			Response->Body = HttpServerResponse.HttpServerResponse.Body;
			Response->Code = HttpServerResponse.HttpServerResponse.Code;
			Response->Headers = HttpServerResponse.HttpServerResponse.Headers;
			Response->HttpVersion = HttpServerResponse.HttpServerResponse.HttpVersion;

			OnComplete(MoveTemp(Response));
			return true;
		}
	}

	TUniquePtr<FHttpServerResponse> response = FHttpServerResponse::Error(EHttpServerResponseCodes::NotFound);
	OnComplete(MoveTemp(response));
	return true;
}

bool USimpleHttpServer::HandleRequestNative(FString HttpPath, const FHttpServerRequest& Request, const FHttpResultCallback& OnComplete)
{
	FNativeHttpServerRequest NativeHttpServerRequest;
	FillNativeRequst(Request, NativeHttpServerRequest);

	if (FHttpRouteHandler* HttpServerRequestDelegate = RouteHandlers.Find(HttpPath))
	{
		(*HttpServerRequestDelegate)(NativeHttpServerRequest);
		return true;
	}

	TUniquePtr<FHttpServerResponse> response = FHttpServerResponse::Error(EHttpServerResponseCodes::NotFound);
	OnComplete(MoveTemp(response));
	return true;
}

void USimpleHttpServer::FillNativeRequst(const FHttpServerRequest& Request, FNativeHttpServerRequest& NativeRequest)
{
	NativeRequest.Verb = (ENativeHttpServerRequestVerbs)Request.Verb;
	NativeRequest.RelativePath = *Request.RelativePath.GetPath();

	for (const auto& Header : Request.Headers)
	{
		FString StrHeaderVals;
		for (const auto& val : Header.Value)
		{
			StrHeaderVals += val + TEXT(" ");
		}

		NativeRequest.Headers.Add(Header.Key, StrHeaderVals);
	}

	NativeRequest.PathParams = Request.PathParams;
	NativeRequest.QueryParams = Request.QueryParams;


	// Convert UTF8 to FString
	FUTF8ToTCHAR BodyTCHARData(reinterpret_cast<const ANSICHAR*>(Request.Body.GetData()), Request.Body.Num());
	FString StrBodyData{ BodyTCHARData.Length(), BodyTCHARData.Get() };

	NativeRequest.Body = *StrBodyData;
}

FNativeHttpServerResponse USimpleHttpServer::MakeResponse(FString Text, FString ContentType, int32 Code)
{
	FNativeHttpServerResponse HttpServerResponse;
	HttpServerResponse.HttpServerResponse.Code = (EHttpServerResponseCodes)Code;

	FTCHARToUTF8 ConvertToUtf8(*Text);
	const uint8* ConvertToUtf8Bytes = (reinterpret_cast<const uint8*>(ConvertToUtf8.Get()));
	HttpServerResponse.HttpServerResponse.Body.Append(ConvertToUtf8Bytes, ConvertToUtf8.Length());

	FString Utf8CharsetContentType = FString::Printf(TEXT("%s;charset=utf-8"), *ContentType);
	TArray<FString> ContentTypeValue = { MoveTemp(Utf8CharsetContentType) };
	HttpServerResponse.HttpServerResponse.Headers.Add(TEXT("content-type"), MoveTemp(ContentTypeValue));

	return HttpServerResponse;
}

UWorld* USimpleHttpServer::GetWorld() const
{
#if WITH_EDITOR
	return GWorld;
#else
	return Super::GetWorld();
#endif
}

void USimpleHttpServer::BindRoutes()
{
	// You can bind any functions for your C++ class
	//BindRoute("/Test", ENativeHttpServerRequestVerbs::GET, [this](FNativeHttpServerRequest HttpServerRequest) {USimpleHttpServer::TestRoute(HttpServerRequest); });

	ReceiveBindRoutes();
}
