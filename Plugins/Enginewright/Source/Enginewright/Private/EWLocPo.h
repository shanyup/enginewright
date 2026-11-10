// Helpers shared by the localization tools: format argument / rich text tokens of a string and a small .po file reader.
#pragma once

#include "CoreMinimal.h"

namespace EWLoc
{
	/** {name} style format arguments and <tag> rich text tags used by a string, sorted. */
	inline TArray<FString> Tokens(const FString& S)
	{
		TArray<FString> Out;
		for (int32 i = 0; i < S.Len(); ++i)
		{
			const TCHAR Open = S[i];
			if (Open != TEXT('{') && Open != TEXT('<')) { continue; }
			if (Open == TEXT('{') && i + 1 < S.Len() && S[i + 1] == TEXT('{')) { ++i; continue; } // escaped
			const TCHAR Close = Open == TEXT('{') ? TEXT('}') : TEXT('>');
			const int32 End = S.Find(FString(1, &Close), ESearchCase::CaseSensitive, ESearchDir::FromStart, i + 1);
			if (End == INDEX_NONE || End - i > 64) { continue; }
			FString Tok = S.Mid(i, End - i + 1);
			if (Open == TEXT('<')) { int32 Sp; if (Tok.FindChar(TEXT(' '), Sp)) { Tok = Tok.Left(Sp) + TEXT(">"); } } // <tag attr="x"> -> <tag>
			Out.Add(Tok);
			i = End;
		}
		Out.Sort();
		return Out;
	}

	/** One entry of a .po file (msgctxt "Namespace,Key", msgid = source text, msgstr = translation). */
	struct FPoItem { FString Ctxt, Id, Str, CommentKey; bool bFuzzy = false; };

	/** Text between the first and the last quote of a po string line, with \n \t \r \" \\ unescaped. */
	inline FString UnquotePo(const FString& In)
	{
		const int32 A = In.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromStart);
		const int32 B = In.Find(TEXT("\""), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (A == INDEX_NONE || B <= A) { return FString(); }
		const FString Body = In.Mid(A + 1, B - A - 1);
		FString Out;
		Out.Reserve(Body.Len());
		for (int32 i = 0; i < Body.Len(); ++i)
		{
			if (Body[i] == TEXT('\\') && i + 1 < Body.Len())
			{
				const TCHAR N = Body[++i];
				Out.AppendChar(N == TEXT('n') ? TEXT('\n') : N == TEXT('t') ? TEXT('\t') : N == TEXT('r') ? TEXT('\r') : N);
			}
			else { Out.AppendChar(Body[i]); }
		}
		return Out;
	}

	inline void ParsePo(const FString& Text, TArray<FPoItem>& Out)
	{
		TArray<FString> Lines;
		Text.ParseIntoArrayLines(Lines, false);
		FPoItem E;
		bool bHave = false;
		int32 Cur = 0; // 1 msgctxt, 2 msgid, 3 msgstr
		auto Flush = [&]() { if (bHave) { Out.Add(E); } E = FPoItem(); bHave = false; Cur = 0; };
		for (FString L : Lines)
		{
			L.TrimStartAndEndInline();
			if (L.IsEmpty()) { Flush(); continue; }
			if (L.StartsWith(TEXT("#~"))) { continue; } // obsolete entry
			if (L.StartsWith(TEXT("#")))
			{
				if (Cur == 3) { Flush(); }
				if (L.StartsWith(TEXT("#,")) && L.Contains(TEXT("fuzzy"))) { E.bFuzzy = true; }
				if (L.StartsWith(TEXT("#.")))
				{
					FString After;
					if (L.Split(TEXT("Key:"), nullptr, &After)) { E.CommentKey = After.TrimStartAndEnd(); }
				}
				continue;
			}
			if (L.StartsWith(TEXT("msgctxt"))) { if (Cur == 3) { Flush(); } Cur = 1; bHave = true; E.Ctxt = UnquotePo(L); }
			else if (L.StartsWith(TEXT("msgid_plural")) || L.StartsWith(TEXT("msgstr["))) { Cur = 0; }
			else if (L.StartsWith(TEXT("msgid"))) { if (Cur == 3) { Flush(); } Cur = 2; bHave = true; E.Id = UnquotePo(L); }
			else if (L.StartsWith(TEXT("msgstr"))) { Cur = 3; E.Str = UnquotePo(L); }
			else if (L.StartsWith(TEXT("\"")))
			{
				if (Cur == 1) { E.Ctxt += UnquotePo(L); } else if (Cur == 2) { E.Id += UnquotePo(L); } else if (Cur == 3) { E.Str += UnquotePo(L); }
			}
		}
		Flush();
	}

	/** Namespace and key of a po entry: UE writes msgctxt "Namespace,Key" and a "#. Key:" comment. */
	inline void SplitContext(const FPoItem& It, FString& Ns, FString& Key)
	{
		if (!It.CommentKey.IsEmpty())
		{
			Key = It.CommentKey;
			Ns = It.Ctxt;
			if (Ns.EndsWith(TEXT(",") + Key)) { Ns.LeftChopInline(Key.Len() + 1); } else if (Ns == Key) { Ns.Empty(); }
			return;
		}
		int32 Comma;
		if (It.Ctxt.FindChar(TEXT(','), Comma)) { Ns = It.Ctxt.Left(Comma); Key = It.Ctxt.Mid(Comma + 1); }
		else { Ns.Empty(); Key = It.Ctxt; }
	}
}
