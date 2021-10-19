// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "AtlasEngine.h"

#include <til/rle.h>

#include "../../interactivity/win32/CustomWindowMessages.h"

#include "shader_vs.h"
#include "shader_ps.h"

#pragma warning(disable : 4100)

using namespace Microsoft::Console::Render;

// Like gsl::narrow but living fast and dying young.
// I don't want to handle users passing fonts larger than 65535pt.
template<typename T, typename U>
constexpr T yolo_narrow(U u) noexcept
{
    const auto t = static_cast<T>(u);
    if (static_cast<U>(t) != u || std::is_signed_v<T> != std::is_signed_v<U> && t < T{} != u < U{})
    {
        FAIL_FAST();
    }
    return t;
}

template<typename T>
constexpr AtlasEngine::vec2<T> yolo_vec2(COORD val) noexcept
{
    return { yolo_narrow<T>(val.X), yolo_narrow<T>(val.Y) };
}

template<typename T>
constexpr AtlasEngine::vec2<T> yolo_vec2(SIZE val) noexcept
{
    return { yolo_narrow<T>(val.cx), yolo_narrow<T>(val.cy) };
}

#define getLocaleName(varName)               \
    wchar_t varName[LOCALE_NAME_MAX_LENGTH]; \
    getLocaleNameImpl(varName);

static void getLocaleNameImpl(wchar_t (&localeName)[LOCALE_NAME_MAX_LENGTH])
{
    if (!GetUserDefaultLocaleName(localeName, LOCALE_NAME_MAX_LENGTH))
    {
        static constexpr wchar_t fallback[] = L"en-US";
        memcpy(localeName, fallback, sizeof(fallback));
    }
    // See: https://docs.microsoft.com/en-us/windows/win32/intl/locale-names
    // "A locale name is based on the language tagging conventions of RFC 4646."
    // That said these locales aren't RFC 4646 as they contain a trailing "_<sort order>".
    // I'm stripping those as I don't want to find out whether something like DWrite can't handle it.
    else if (auto p = wcschr(localeName, L'_'))
    {
        *p = L'\0';
    }
}

struct TextAnalyzer final : IDWriteTextAnalysisSource, IDWriteTextAnalysisSink
{
    constexpr TextAnalyzer(const std::wstring& text, const wchar_t* localeName, std::vector<AtlasEngine::TextAnalyzerResult>& results) noexcept :
        _text(text), _localeName(localeName), _results(results) {}

    void Analyze(IDWriteTextAnalyzer* textAnalyzer, UINT32 textPosition, UINT32 textLength)
    {
        _results.clear();
        textAnalyzer->AnalyzeScript(this, textPosition, textLength, this);
        //textAnalyzer->AnalyzeBidi(this, textPosition, textLength, this);
    }

    HRESULT __stdcall QueryInterface(REFIID riid, _COM_Outptr_ void** ppvObject) noexcept override
    {
        if (IsEqualGUID(riid, __uuidof(IDWriteTextAnalysisSource)) || IsEqualGUID(riid, __uuidof(IDWriteTextAnalysisSink)))
        {
            *ppvObject = this;
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG __stdcall AddRef() noexcept override
    {
        assert(false);
        return 1;
    }

    ULONG __stdcall Release() noexcept override
    {
        assert(false);
        return 1;
    }

    HRESULT __stdcall GetTextAtPosition(UINT32 textPosition, _Outptr_result_buffer_(*textLength) WCHAR const** textString, _Out_ UINT32* textLength) noexcept override
    {
        *textString = _text.data() + textPosition;
        *textLength = static_cast<UINT32>(_text.size() - textPosition);
        return S_OK;
    }

    HRESULT __stdcall GetTextBeforePosition(UINT32 textPosition, _Outptr_result_buffer_(*textLength) WCHAR const** textString, _Out_ UINT32* textLength) noexcept override
    {
        *textString = _text.data();
        *textLength = textPosition;
        return S_OK;
    }

    DWRITE_READING_DIRECTION __stdcall GetParagraphReadingDirection() noexcept override
    {
        return DWRITE_READING_DIRECTION_LEFT_TO_RIGHT;
    }

    HRESULT __stdcall GetLocaleName(UINT32 textPosition, _Out_ UINT32* textLength, _Outptr_result_z_ WCHAR const** localeName) noexcept override
    {
        *textLength = gsl::narrow_cast<UINT32>(_text.size()) - textPosition;
        *localeName = _localeName;
        return S_OK;
    }

    HRESULT __stdcall GetNumberSubstitution(UINT32 textPosition, _Out_ UINT32* textLength, _COM_Outptr_ IDWriteNumberSubstitution** numberSubstitution) noexcept override
    {
        *textLength = gsl::narrow_cast<UINT32>(_text.size()) - textPosition;
        *numberSubstitution = nullptr;
        return E_NOTIMPL;
    }

    HRESULT __stdcall SetScriptAnalysis(UINT32 textPosition, UINT32 textLength, _In_ DWRITE_SCRIPT_ANALYSIS const* scriptAnalysis) noexcept override
    try
    {
        _results.emplace_back(AtlasEngine::TextAnalyzerResult{ textPosition, textLength, scriptAnalysis->script, static_cast<UINT8>(scriptAnalysis->shapes), 0 });
        return S_OK;
    }
    CATCH_RETURN()

    HRESULT __stdcall SetLineBreakpoints(UINT32 textPosition, UINT32 textLength, _In_reads_(textLength) DWRITE_LINE_BREAKPOINT const* lineBreakpoints) noexcept override
    {
        return E_NOTIMPL;
    }

    HRESULT __stdcall SetBidiLevel(UINT32 textPosition, UINT32 textLength, UINT8 explicitLevel, UINT8 resolvedLevel) noexcept override
    {
        return E_NOTIMPL;
    }

    HRESULT __stdcall SetNumberSubstitution(UINT32 textPosition, UINT32 textLength, _In_ IDWriteNumberSubstitution* numberSubstitution) noexcept override
    {
        return E_NOTIMPL;
    }

private:
    const std::wstring& _text;
    const wchar_t* const _localeName;
    std::vector<AtlasEngine::TextAnalyzerResult>& _results;
};

AtlasEngine::AtlasEngine()
{
    THROW_IF_FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, _uuidof(_sr.d2dFactory), reinterpret_cast<void**>(_sr.d2dFactory.addressof())));
    THROW_IF_FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(_sr.dwriteFactory), reinterpret_cast<::IUnknown**>(_sr.dwriteFactory.addressof())));
    THROW_IF_FAILED(_sr.dwriteFactory.query<IDWriteFactory2>()->GetSystemFontFallback(_sr.systemFontFallback.addressof()));
    {
        wil::com_ptr<IDWriteTextAnalyzer> textAnalyzer;
        THROW_IF_FAILED(_sr.dwriteFactory->CreateTextAnalyzer(textAnalyzer.addressof()));
        _sr.textAnalyzer = textAnalyzer.query<IDWriteTextAnalyzer1>();
    }
    _sr.isWindows10OrGreater = IsWindows10OrGreater();

    _r.glyphQueue.reserve(64);
}

#pragma region IRenderEngine

// StartPaint() is called while the console buffer lock is being held.
// --> Put as little in here as possible.
[[nodiscard]] HRESULT AtlasEngine::StartPaint() noexcept
try
{
    if (_api.hwnd)
    {
        RECT rect;
        LOG_IF_WIN32_BOOL_FALSE(GetClientRect(_api.hwnd, &rect));
        (void)SetWindowSize({ rect.right - rect.left, rect.bottom - rect.top });

        if (WI_IsFlagSet(_invalidations, invalidation_flags::title))
        {
            LOG_IF_WIN32_BOOL_FALSE(PostMessageW(_api.hwnd, CM_UPDATE_TITLE, 0, 0));
            WI_ClearFlag(_invalidations, invalidation_flags::title);
        }
    }

    // It's important that we invalidate here instead of in Present() with the rest.
    // Other functions, those called before Present(), might depend on _r fields.
    // But most of the time _invalidations will be ::none, making this very cheap.
    if (_invalidations != invalidation_flags::none)
    {
        FAIL_FAST_IF(_api.sizeInPixel == u16x2{} || _api.cellSize == u16x2{} || _api.cellCount == u16x2{});

        if (WI_IsFlagSet(_invalidations, invalidation_flags::device))
        {
            _createResources();
            WI_ClearFlag(_invalidations, invalidation_flags::device);
        }
        if (WI_IsFlagSet(_invalidations, invalidation_flags::size))
        {
            _recreateSizeDependentResources();
            WI_ClearFlag(_invalidations, invalidation_flags::size);
        }
        if (WI_IsFlagSet(_invalidations, invalidation_flags::font))
        {
            _recreateFontDependentResources();
            WI_ClearFlag(_invalidations, invalidation_flags::font);
        }
    }

    _rapi.dirtyArea = til::rectangle{ 0u, 0u, static_cast<size_t>(_api.cellCount.x), static_cast<size_t>(_api.cellCount.y) };
    return S_OK;
}
catch (const wil::ResultException& exception)
{
    return _handleException(exception);
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::EndPaint() noexcept
{
    return S_OK;
}

[[nodiscard]] bool AtlasEngine::RequiresContinuousRedraw() noexcept
{
    return false;
}

void AtlasEngine::WaitUntilCanRender() noexcept
{
    if (_r.frameLatencyWaitableObject)
    {
        WaitForSingleObjectEx(_r.frameLatencyWaitableObject.get(), 1000, true);
    }
    else
    {
        Sleep(8);
    }
}

// Present() is called without the console buffer lock being held.
// --> Put as much in here as possible.
[[nodiscard]] HRESULT AtlasEngine::Present() noexcept
try
{
    if (!_r.glyphQueue.empty())
    {
        for (const auto& pair : _r.glyphQueue)
        {
            _drawGlyph(pair);
        }
        _r.glyphQueue.clear();
    }

    // The values the constant buffer depends on are potentially updated after BeginPaint().
    if (WI_IsFlagSet(_invalidations, invalidation_flags::cbuffer))
    {
        _updateConstantBuffer();
        WI_ClearFlag(_invalidations, invalidation_flags::cbuffer);
    }

    {
#pragma warning(suppress : 26494) // Variable 'mapped' is uninitialized. Always initialize an object (type.5).
        D3D11_MAPPED_SUBRESOURCE mapped;
        THROW_IF_FAILED(_r.deviceContext->Map(_r.cellBuffer.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped));
        assert(mapped.RowPitch >= _r.cells.size() * sizeof(cell));
        memcpy(mapped.pData, _r.cells.data(), _r.cells.size() * sizeof(cell));
        _r.deviceContext->Unmap(_r.cellBuffer.get(), 0);
    }

    // After Present calls, the back buffer needs to explicitly be
    // re-bound to the D3D11 immediate context before it can be used again.
    _r.deviceContext->OMSetRenderTargets(1, _r.renderTargetView.addressof(), nullptr);
    _r.deviceContext->Draw(3, 0);

    THROW_IF_FAILED(_r.swapChain->Present(1, 0));

    // On some GPUs with tile based deferred rendering (TBDR) architectures, binding
    // RenderTargets that already have contents in them (from previous rendering) incurs a
    // cost for having to copy the RenderTarget contents back into tile memory for rendering.
    //
    // On Windows 10 with DXGI_SWAP_EFFECT_FLIP_DISCARD we get this for free.
    if (!_sr.isWindows10OrGreater)
    {
        _r.deviceContext->DiscardView(_r.renderTargetView.get());
    }

    return S_OK;
}
catch (const wil::ResultException& exception)
{
    return _handleException(exception);
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::PrepareForTeardown(_Out_ bool* const pForcePaint) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pForcePaint);
    *pForcePaint = false;
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::ScrollFrame() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::Invalidate(const SMALL_RECT* const psrRegion) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::InvalidateCursor(const SMALL_RECT* const psrRegion) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::InvalidateSystem(const RECT* const prcDirtyClient) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::InvalidateSelection(const std::vector<SMALL_RECT>& rectangles) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::InvalidateScroll(const COORD* const pcoordDelta) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::InvalidateAll() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::InvalidateCircling(_Out_ bool* const pForcePaint) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pForcePaint);
    *pForcePaint = false;
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::InvalidateTitle(const std::wstring_view proposedTitle) noexcept
{
    WI_SetFlag(_invalidations, invalidation_flags::title);
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PrepareRenderInfo(const RenderFrameInfo& info) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::ResetLineTransform() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PrepareLineTransform(const LineRendition lineRendition, const size_t targetRow, const size_t viewportLeft) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PaintBackground() noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PaintBufferLine(const gsl::span<const Cluster> clusters, const COORD coord, const bool fTrimLeft, const bool lineWrapped) noexcept
try
{
    {
        _rapi.bufferLine.clear();
        _rapi.bufferLinePos.clear();

        u16 column = 0;
        for (const auto& cluster : clusters)
        {
            const auto text = cluster.GetText();

            _rapi.bufferLine.append(text);
            for (size_t i = 0; i < text.size(); ++i)
            {
                _rapi.bufferLinePos.emplace_back(column);
            }

            column += gsl::narrow_cast<u16>(cluster.GetColumns());
        }

        _rapi.bufferLinePos.emplace_back(column);
    }

    _processBufferLine(coord.Y);
    return S_OK;
}
CATCH_RETURN()

[[nodiscard]] HRESULT AtlasEngine::PaintBufferGridLines(const GridLineSet lines, const COLORREF color, const size_t cchLine, const COORD coordTarget) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PaintSelection(const SMALL_RECT rect) noexcept
{
    const auto width = rect.Right - rect.Left;
    auto row = _getCell(rect.Left, rect.Top);

    for (auto x = rect.Top, x1 = rect.Bottom; x < x1; ++x, row += _api.cellCount.x)
    {
        for (auto data = row, dataEnd = row + width; data != dataEnd; ++data)
        {
            data->flags |= 2;
        }
    }

    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::PaintCursor(const CursorOptions& options) noexcept
{
    if (options.isOn)
    {
        auto data = _getCell(options.coordCursor.X, options.coordCursor.Y);
        const auto end = std::min(data + options.fIsDoubleWidth + 1, _r.cells.data() + _r.cells.size());

        for (; data != end; ++data)
        {
            data->flags |= 1;
        }
    }

    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::UpdateDrawingBrushes(const TextAttribute& textAttributes, const gsl::not_null<IRenderData*> pData, const bool usingSoftFont, const bool isSettingDefaultBrushes) noexcept
{
    const auto [fg, bg] = pData->GetAttributeColors(textAttributes);

    if (!isSettingDefaultBrushes)
    {
        _rapi.currentColor = { fg, bg };
        _rapi.attributes.bold = textAttributes.IsBold();
        _rapi.attributes.italic = textAttributes.IsItalic();
    }
    else if (textAttributes.BackgroundIsDefault() && bg != _rapi.backgroundColor)
    {
        _rapi.backgroundColor = bg;
        WI_SetFlag(_invalidations, invalidation_flags::cbuffer);
    }

    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::UpdateFont(const FontInfoDesired& fontInfoDesired, _Out_ FontInfo& fontInfo) noexcept
{
    return UpdateFont(fontInfoDesired, fontInfo, {}, {});
}

[[nodiscard]] HRESULT AtlasEngine::UpdateSoftFont(const gsl::span<const uint16_t> bitPattern, const SIZE cellSize, const size_t centeringHint) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::UpdateDpi(const int dpi) noexcept
{
    const auto newDPI = yolo_narrow<u16>(dpi);
    if (_api.dpi != newDPI)
    {
        _api.dpi = newDPI;
        WI_SetFlag(_invalidations, invalidation_flags::font);
    }
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::UpdateViewport(const SMALL_RECT srNewViewport) noexcept
{
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::GetProposedFont(const FontInfoDesired& fontInfoDesired, _Out_ FontInfo& fontInfo, const int dpi) noexcept
{
    const auto scaling = GetScaling();
    const auto coordFontRequested = fontInfoDesired.GetEngineSize();
    wil::unique_hfont hfont;
    COORD coordSize;

    // This block of code (for GDI fonts) is unfinished.
    if (fontInfoDesired.IsDefaultRasterFont())
    {
        hfont.reset(static_cast<HFONT>(GetStockObject(OEM_FIXED_FONT)));
        RETURN_HR_IF(E_FAIL, !hfont);
    }
    else if (fontInfoDesired.GetFaceName() == DEFAULT_RASTER_FONT_FACENAME)
    {
        // For future reference, here is the engine weighting and internal details on Windows Font Mapping:
        // https://msdn.microsoft.com/en-us/library/ms969909.aspx
        // More relevant links:
        // https://support.microsoft.com/en-us/kb/94646

        LOGFONTW lf;
        lf.lfHeight = yolo_narrow<LONG>(std::ceil(coordFontRequested.Y * scaling));
        lf.lfWidth = 0;
        lf.lfEscapement = 0;
        lf.lfOrientation = 0;
        lf.lfWeight = fontInfoDesired.GetWeight();
        lf.lfItalic = FALSE;
        lf.lfUnderline = FALSE;
        lf.lfStrikeOut = FALSE;
        lf.lfCharSet = OEM_CHARSET;
        lf.lfOutPrecision = OUT_RASTER_PRECIS;
        lf.lfClipPrecision = CLIP_DEFAULT_PRECIS;
        lf.lfQuality = PROOF_QUALITY;
        lf.lfPitchAndFamily = FIXED_PITCH | FF_MODERN;
        wmemcpy(lf.lfFaceName, DEFAULT_RASTER_FONT_FACENAME, std::size(DEFAULT_RASTER_FONT_FACENAME));

        hfont.reset(CreateFontIndirectW(&lf));
        RETURN_HR_IF(E_FAIL, !hfont);
    }

    if (hfont)
    {
        wil::unique_hdc hdc(CreateCompatibleDC(nullptr));
        RETURN_HR_IF(E_FAIL, !hdc);

        DeleteObject(SelectObject(hdc.get(), hfont.get()));

        SIZE sz;
        RETURN_HR_IF(E_FAIL, !GetTextExtentPoint32W(hdc.get(), L"M", 1, &sz));

        coordSize.X = yolo_narrow<SHORT>(sz.cx);
        coordSize.Y = yolo_narrow<SHORT>(sz.cy);
    }
    else
    {
        getLocaleName(localeName);

        const auto textFormat = _createTextFormat(
            fontInfoDesired.GetFaceName().c_str(),
            static_cast<DWRITE_FONT_WEIGHT>(fontInfoDesired.GetWeight()),
            DWRITE_FONT_STYLE_NORMAL,
            fontInfoDesired.GetEngineSize().Y,
            localeName);

        wil::com_ptr<IDWriteTextLayout> textLayout;
        RETURN_IF_FAILED(_sr.dwriteFactory->CreateTextLayout(L"M", 1, textFormat.get(), FLT_MAX, FLT_MAX, textLayout.addressof()));

        DWRITE_TEXT_METRICS metrics;
        RETURN_IF_FAILED(textLayout->GetMetrics(&metrics));

        coordSize.X = yolo_narrow<SHORT>(std::ceil(metrics.width * scaling));
        coordSize.Y = yolo_narrow<SHORT>(std::ceil(metrics.height * scaling));
    }

    fontInfo.SetFromEngine(
        fontInfoDesired.GetFaceName(),
        fontInfoDesired.GetFamily(),
        fontInfoDesired.GetWeight(),
        false,
        coordSize,
        fontInfoDesired.GetEngineSize());
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::GetDirtyArea(gsl::span<const til::rectangle>& area) noexcept
{
    area = gsl::span{ &_rapi.dirtyArea, 1 };
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::GetFontSize(_Out_ COORD* const pFontSize) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pFontSize);
    pFontSize->X = gsl::narrow_cast<SHORT>(_api.cellSize.x);
    pFontSize->Y = gsl::narrow_cast<SHORT>(_api.cellSize.y);
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::IsGlyphWideByFont(const std::wstring_view glyph, _Out_ bool* const pResult) noexcept
{
    RETURN_HR_IF_NULL(E_INVALIDARG, pResult);

    wil::com_ptr<IDWriteTextLayout> textLayout;
    RETURN_IF_FAILED(_sr.dwriteFactory->CreateTextLayout(glyph.data(), yolo_narrow<uint32_t>(glyph.size()), _getTextFormat(false, false), FLT_MAX, FLT_MAX, textLayout.addressof()));

    DWRITE_TEXT_METRICS metrics;
    RETURN_IF_FAILED(textLayout->GetMetrics(&metrics));

    *pResult = static_cast<unsigned int>(std::ceil(metrics.width)) > _api.cellSize.x;
    return S_OK;
}

[[nodiscard]] HRESULT AtlasEngine::UpdateTitle(const std::wstring_view newTitle) noexcept
{
    return S_OK;
}

#pragma endregion

#pragma region DxRenderer

[[nodiscard]] bool AtlasEngine::GetRetroTerminalEffect() const noexcept
{
    return false;
}

[[nodiscard]] float AtlasEngine::GetScaling() const noexcept
{
    return static_cast<float>(_api.dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
}

[[nodiscard]] HANDLE AtlasEngine::GetSwapChainHandle()
{
    if (!_r.device)
    {
        _createResources();
    }
    return _r.swapChainHandle.get();
}

[[nodiscard]] Microsoft::Console::Types::Viewport AtlasEngine::GetViewportInCharacters(const Types::Viewport& viewInPixels) const noexcept
{
    return Types::Viewport::FromDimensions(viewInPixels.Origin(), COORD{ gsl::narrow_cast<short>(viewInPixels.Width() / _api.cellSize.x), gsl::narrow_cast<short>(viewInPixels.Height() / _api.cellSize.y) });
}

[[nodiscard]] Microsoft::Console::Types::Viewport AtlasEngine::GetViewportInPixels(const Types::Viewport& viewInCharacters) const noexcept
{
    return Types::Viewport::FromDimensions(viewInCharacters.Origin(), COORD{ gsl::narrow_cast<short>(viewInCharacters.Width() * _api.cellSize.x), gsl::narrow_cast<short>(viewInCharacters.Height() * _api.cellSize.y) });
}

void AtlasEngine::SetAntialiasingMode(const D2D1_TEXT_ANTIALIAS_MODE antialiasingMode) noexcept
{
    _api.antialiasingMode = yolo_narrow<u16>(antialiasingMode);
    WI_SetFlag(_invalidations, invalidation_flags::font);
}

void AtlasEngine::SetCallback(std::function<void()> pfn)
{
    _api.swapChainChangedCallback = std::move(pfn);
}

void AtlasEngine::SetDefaultTextBackgroundOpacity(const float opacity) noexcept
{
}

void AtlasEngine::SetForceFullRepaintRendering(bool enable) noexcept
{
}

[[nodiscard]] HRESULT AtlasEngine::SetHwnd(const HWND hwnd) noexcept
{
    _api.hwnd = hwnd;
    return S_OK;
}

void AtlasEngine::SetPixelShaderPath(std::wstring_view value) noexcept
{
}

void AtlasEngine::SetRetroTerminalEffect(bool enable) noexcept
{
}

void AtlasEngine::SetSelectionBackground(const COLORREF color, const float alpha) noexcept
{
    const u32 selectionColor = color | static_cast<u32>(std::lroundf(alpha * 255.0f)) << 24;

    if (_rapi.selectionColor != selectionColor)
    {
        _rapi.selectionColor = selectionColor;
        WI_SetFlag(_invalidations, invalidation_flags::cbuffer);
    }
}

void AtlasEngine::SetSoftwareRendering(bool enable) noexcept
{
}

void AtlasEngine::SetWarningCallback(std::function<void(const HRESULT)> pfn)
{
}

[[nodiscard]] HRESULT AtlasEngine::SetWindowSize(const SIZE pixels) noexcept
{
    // At the time of writing:
    // When Win+D is pressed a render pass is initiated. As conhost is in the background, GetClientRect will return {0,0}.
    // This isn't a valid value for _api.sizeInPixel and would crash _recreateSizeDependentResources().
    if (const auto newSize = yolo_vec2<u16>(pixels); _api.sizeInPixel != newSize && newSize != u16x2{})
    {
        _api.sizeInPixel = newSize;
        _api.cellCount = _api.sizeInPixel / _api.cellSize;
        WI_SetFlag(_invalidations, invalidation_flags::size);
    }

    return S_OK;
}

void AtlasEngine::ToggleShaderEffects()
{
}

[[nodiscard]] HRESULT AtlasEngine::UpdateFont(const FontInfoDesired& fontInfoDesired, FontInfo& fontInfo, const std::unordered_map<std::wstring_view, uint32_t>& features, const std::unordered_map<std::wstring_view, float>& axes) noexcept
try
{
    RETURN_IF_FAILED(GetProposedFont(fontInfoDesired, fontInfo, _api.dpi));

    _api.fontSize = fontInfoDesired.GetEngineSize().Y;
    _api.fontName = fontInfo.GetFaceName();
    _api.fontWeight = yolo_narrow<u16>(fontInfo.GetWeight());

    WI_SetFlag(_invalidations, invalidation_flags::font);

    if (const auto newSize = yolo_vec2<u16>(fontInfo.GetSize()); _api.cellSize != newSize)
    {
        const auto scaling = GetScaling();
        _api.cellSizeDIP.x = static_cast<float>(newSize.x) / scaling;
        _api.cellSizeDIP.y = static_cast<float>(newSize.y) / scaling;
        _api.cellSize = newSize;
        _api.cellCount = _api.sizeInPixel / _api.cellSize;
        WI_SetFlag(_invalidations, invalidation_flags::size);
    }

    return S_OK;
}
CATCH_RETURN()

void AtlasEngine::UpdateHyperlinkHoveredId(const uint16_t hoveredId) noexcept
{
}

#pragma endregion

#pragma region Helper classes

// XXH3 for exactly 32 bytes.
uint64_t AtlasEngine::XXH3_len_32_64b(const void* data) noexcept
{
    static constexpr uint64_t dataSize = 32;
    static constexpr auto XXH3_mul128_fold64 = [](uint64_t lhs, uint64_t rhs) noexcept {
        uint64_t lo, hi;

#if defined(_M_AMD64)
        lo = _umul128(lhs, rhs, &hi);
#elif defined(_M_ARM64)
        lo = lhs * rhs;
        hi = __umulh(lhs, rhs);
#else
        const uint64_t lo_lo = __emulu(lhs, rhs);
        const uint64_t hi_lo = __emulu(lhs >> 32, rhs);
        const uint64_t lo_hi = __emulu(lhs, rhs >> 32);
        const uint64_t hi_hi = __emulu(lhs >> 32, rhs >> 32);
        const uint64_t cross = (lo_lo >> 32) + (hi_lo & 0xFFFFFFFF) + lo_hi;
        hi = (hi_lo >> 32) + (cross >> 32) + hi_hi;
        lo = (cross << 32) | (lo_lo & 0xFFFFFFFF);
#endif

        return lo ^ hi;
    };

    // If executed on little endian CPUs these 4 numbers will
    // equal the first 32 byte of the original XXH3_kSecret.
    static constexpr uint64_t XXH3_kSecret[4] = {
        0xbe4ba423396cfeb8ull,
        0x1cad21f72c81017cull,
        0xdb979083e96dd4deull,
        0x1f67b3b7a4a44072ull,
    };

    uint64_t inputs[4];
    memcpy(&inputs[0], data, 32);

    uint64_t acc = dataSize * 0x9E3779B185EBCA87ull;
    acc += XXH3_mul128_fold64(inputs[0] ^ XXH3_kSecret[0], inputs[1] ^ XXH3_kSecret[1]);
    acc += XXH3_mul128_fold64(inputs[2] ^ XXH3_kSecret[2], inputs[3] ^ XXH3_kSecret[3]);
    acc = acc ^ (acc >> 37);
    acc *= 0x165667919E3779F9ULL;
    acc = acc ^ (acc >> 32);
    return acc;
}

#pragma endregion

[[nodiscard]] HRESULT AtlasEngine::_handleException(const wil::ResultException& exception) noexcept
{
    const auto hr = exception.GetErrorCode();
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == D2DERR_RECREATE_TARGET)
    {
        _r = {};
        WI_SetFlag(_invalidations, invalidation_flags::device);
        return E_PENDING; // Indicate a retry to the renderer
    }
    return hr;
}

void AtlasEngine::_createResources()
{
#ifdef NDEBUG
    static constexpr
#endif
        auto deviceFlags = D3D11_CREATE_DEVICE_SINGLETHREADED | D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

#ifndef NDEBUG
    // DXGI debug messages + enabling D3D11_CREATE_DEVICE_DEBUG if the Windows SDK was installed.
    if (const wil::unique_hmodule module{ LoadLibraryExW(L"dxgidebug.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) })
    {
        deviceFlags |= D3D11_CREATE_DEVICE_DEBUG;

        const auto DXGIGetDebugInterface = reinterpret_cast<HRESULT(WINAPI*)(REFIID, void**)>(GetProcAddress(module.get(), "DXGIGetDebugInterface"));
        THROW_LAST_ERROR_IF(!DXGIGetDebugInterface);

        wil::com_ptr<IDXGIInfoQueue> infoQueue;
        if (SUCCEEDED(DXGIGetDebugInterface(IID_PPV_ARGS(infoQueue.addressof()))))
        {
            // I didn't want to link with dxguid.lib just for getting DXGI_DEBUG_ALL.
            static constexpr GUID dxgiDebugAll = { 0xe48ae283, 0xda80, 0x490b, { 0x87, 0xe6, 0x43, 0xe9, 0xa9, 0xcf, 0xda, 0x8 } };
            for (const auto severity : std::array{ DXGI_INFO_QUEUE_MESSAGE_SEVERITY_CORRUPTION, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR, DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING })
            {
                infoQueue->SetBreakOnSeverity(dxgiDebugAll, severity, true);
            }
        }
    }
#endif // NDEBUG

    // D3D device setup (basically a D3D class factory)
    {
        wil::com_ptr<ID3D11DeviceContext> deviceContext;

        static constexpr std::array driverTypes{
            D3D_DRIVER_TYPE_HARDWARE,
            D3D_DRIVER_TYPE_WARP,
        };
        static constexpr std::array featureLevels{
            D3D_FEATURE_LEVEL_11_1,
            D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1,
        };

        HRESULT hr = S_OK;
        for (const auto driverType : driverTypes)
        {
            hr = D3D11CreateDevice(
                /* pAdapter */ nullptr,
                /* DriverType */ driverType,
                /* Software */ nullptr,
                /* Flags */ deviceFlags,
                /* pFeatureLevels */ featureLevels.data(),
                /* FeatureLevels */ gsl::narrow_cast<UINT>(featureLevels.size()),
                /* SDKVersion */ D3D11_SDK_VERSION,
                /* ppDevice */ _r.device.put(),
                /* pFeatureLevel */ nullptr,
                /* ppImmediateContext */ deviceContext.put());
            if (SUCCEEDED(hr))
            {
                break;
            }
        }
        THROW_IF_FAILED(hr);

        _r.deviceContext = deviceContext.query<ID3D11DeviceContext1>();
    }

#ifndef NDEBUG
    // D3D debug messages
    if (deviceFlags & D3D11_CREATE_DEVICE_DEBUG)
    {
        const auto infoQueue = _r.device.query<ID3D11InfoQueue>();
        for (const auto severity : std::array{ D3D11_MESSAGE_SEVERITY_CORRUPTION, D3D11_MESSAGE_SEVERITY_ERROR, D3D11_MESSAGE_SEVERITY_WARNING })
        {
            infoQueue->SetBreakOnSeverity(severity, true);
        }
    }
#endif // NDEBUG

    // D3D swap chain setup (the thing that allows us to present frames on the screen)
    {
        const auto supportsFrameLatencyWaitableObject = IsWindows8Point1OrGreater();

        // With C++20 we'll finally have designated initializers.
        DXGI_SWAP_CHAIN_DESC1 desc{};
        desc.Width = _api.sizeInPixel.x;
        desc.Height = _api.sizeInPixel.y;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.Scaling = DXGI_SCALING_NONE;
        desc.SwapEffect = _sr.isWindows10OrGreater ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        desc.Flags = supportsFrameLatencyWaitableObject ? DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT : 0;

        wil::com_ptr<IDXGIFactory2> dxgiFactory;
        THROW_IF_FAILED(CreateDXGIFactory1(IID_PPV_ARGS(dxgiFactory.addressof())));

        if (_api.hwnd)
        {
            if (FAILED(dxgiFactory->CreateSwapChainForHwnd(_r.device.get(), _api.hwnd, &desc, nullptr, nullptr, _r.swapChain.put())))
            {
                desc.Scaling = DXGI_SCALING_STRETCH;
                THROW_IF_FAILED(dxgiFactory->CreateSwapChainForHwnd(_r.device.get(), _api.hwnd, &desc, nullptr, nullptr, _r.swapChain.put()));
            }
        }
        else
        {
            // We can't link with dcomp.lib, as dcomp.dll doesn't exist on Windows 7.
            const wil::unique_hmodule module{ LoadLibraryExW(L"dcomp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32) };
            THROW_LAST_ERROR_IF(!module);

            const auto DCompositionCreateSurfaceHandle = reinterpret_cast<HRESULT(WINAPI*)(DWORD, SECURITY_ATTRIBUTES*, HANDLE*)>(GetProcAddress(module.get(), "DCompositionCreateSurfaceHandle"));
            THROW_LAST_ERROR_IF(!DCompositionCreateSurfaceHandle);

            // As per: https://docs.microsoft.com/en-us/windows/win32/api/dcomp/nf-dcomp-dcompositioncreatesurfacehandle
            static constexpr DWORD COMPOSITIONSURFACE_ALL_ACCESS = 0x0003L;
            THROW_IF_FAILED(DCompositionCreateSurfaceHandle(COMPOSITIONSURFACE_ALL_ACCESS, nullptr, _r.swapChainHandle.put()));
            THROW_IF_FAILED(dxgiFactory.query<IDXGIFactoryMedia>()->CreateSwapChainForCompositionSurfaceHandle(_r.device.get(), _r.swapChainHandle.get(), &desc, nullptr, _r.swapChain.put()));
        }

        if (supportsFrameLatencyWaitableObject)
        {
            _r.frameLatencyWaitableObject.reset(_r.swapChain.query<IDXGISwapChain2>()->GetFrameLatencyWaitableObject());
            THROW_LAST_ERROR_IF(!_r.frameLatencyWaitableObject);
        }
    }

    // Our constant buffer will never get resized
    {
        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = sizeof(const_buffer);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        THROW_IF_FAILED(_r.device->CreateBuffer(&desc, nullptr, _r.constantBuffer.put()));
    }

    THROW_IF_FAILED(_r.device->CreateVertexShader(shader_vs, sizeof(shader_vs), nullptr, _r.vertexShader.put()));
    THROW_IF_FAILED(_r.device->CreatePixelShader(shader_ps, sizeof(shader_ps), nullptr, _r.pixelShader.put()));

    if (_api.swapChainChangedCallback)
    {
        try
        {
            _api.swapChainChangedCallback();
        }
        CATCH_LOG();
    }

    WI_SetAllFlags(_invalidations, invalidation_flags::size | invalidation_flags::font | invalidation_flags::cbuffer);
}

void AtlasEngine::_recreateSizeDependentResources()
{
    // ResizeBuffer() docs:
    //   Before you call ResizeBuffers, ensure that the application releases all references [...].
    //   You can use ID3D11DeviceContext::ClearState to ensure that all [internal] references are released.
    _r.renderTargetView.reset();
    _r.deviceContext->ClearState();

    THROW_IF_FAILED(_r.swapChain->ResizeBuffers(0, _api.sizeInPixel.x, _api.sizeInPixel.y, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));

    // The RenderTargetView is later used with OMSetRenderTargets
    // to tell D3D where stuff is supposed to be rendered at.
    {
        wil::com_ptr<ID3D11Texture2D> buffer;
        THROW_IF_FAILED(_r.swapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), buffer.put_void()));
        THROW_IF_FAILED(_r.device->CreateRenderTargetView(buffer.get(), nullptr, _r.renderTargetView.put()));
    }

    // Tell D3D which parts of the render target will be visible.
    // Everything outside of the viewport will be black.
    //
    // In the future this should cover the entire _api.sizeInPixel.x/_api.sizeInPixel.y.
    // The pixel shader should draw the remaining content in the configured background color.
    {
        D3D11_VIEWPORT viewport{};
        viewport.Width = static_cast<float>(_api.sizeInPixel.x);
        viewport.Height = static_cast<float>(_api.sizeInPixel.y);
        _r.deviceContext->RSSetViewports(1, &viewport);
    }

    if (const auto totalCellCount = _api.cellCount.area<size_t>(); totalCellCount != _r.cells.size())
    {
        // Prevent a memory usage spike, by first deallocating and then allocating.
        _r.cells = {};
        // Our render loop heavily relies on memcpy() which is between 1.5x
        // and 40x as fast for allocations with an alignment of 32 or greater.
        // (AMD Zen1-3 have a rep movsb performance bug for certain unaligned allocations.)
        _r.cells = aligned_buffer<cell>{ totalCellCount, 32 };

        D3D11_BUFFER_DESC desc{};
        desc.ByteWidth = _api.cellCount.x * _api.cellCount.y * sizeof(cell);
        desc.Usage = D3D11_USAGE_DYNAMIC;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        desc.StructureByteStride = sizeof(cell);
        THROW_IF_FAILED(_r.device->CreateBuffer(&desc, nullptr, _r.cellBuffer.put()));
        THROW_IF_FAILED(_r.device->CreateShaderResourceView(_r.cellBuffer.get(), nullptr, _r.cellView.put()));
    }

    // We have called _r.deviceContext->ClearState() in the beginning and lost all D3D state.
    // This forces us to set up everything up from scratch again.
    {
        _r.deviceContext->VSSetShader(_r.vertexShader.get(), nullptr, 0);
        _r.deviceContext->PSSetShader(_r.pixelShader.get(), nullptr, 0);

        // Our vertex shader uses a trick from Bill Bilodeau published in
        // "Vertex Shader Tricks" at GDC14 to draw a fullscreen triangle
        // without vertex/index buffers. This prepares our context for this.
        _r.deviceContext->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
        _r.deviceContext->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);
        _r.deviceContext->IASetInputLayout(nullptr);
        _r.deviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        _r.deviceContext->PSSetConstantBuffers(0, 1, _r.constantBuffer.addressof());

        _setShaderResources();
    }

    WI_SetFlag(_invalidations, invalidation_flags::cbuffer);
}

void AtlasEngine::_recreateFontDependentResources()
{
    {
        static constexpr size_t wantCells = 64 * 1024; // TODO

        const size_t maxSize = _r.device->GetFeatureLevel() >= D3D_FEATURE_LEVEL_11_0 ? D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION : D3D10_REQ_TEXTURE2D_U_OR_V_DIMENSION;
        const size_t csx = _api.cellSize.x;
        const auto xFit = std::min(wantCells, maxSize / csx);
        const auto yFit = (wantCells + xFit - 1) / xFit;

        _r.glyphs = {};
        _r.glyphQueue = {};
        _r.atlasSizeInPixel = _api.cellSize * u16x2{ yolo_narrow<u16>(xFit), yolo_narrow<u16>(yFit) };
        // The first cell at {0, 0} is always our cursor texture.
        // --> The first glyph starts at {1, 0}.
        _r.atlasPosition = _api.cellSize * u16x2{ 1, 0 };
    }

    // D3D
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = _r.atlasSizeInPixel.x;
        desc.Height = _r.atlasSizeInPixel.y;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = { 1, 0 };
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        THROW_IF_FAILED(_r.device->CreateTexture2D(&desc, nullptr, _r.glyphBuffer.put()));
        THROW_IF_FAILED(_r.device->CreateShaderResourceView(_r.glyphBuffer.get(), nullptr, _r.glyphView.put()));
    }
    {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = _api.cellSize.x * 16;
        desc.Height = _api.cellSize.y;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc = { 1, 0 };
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        THROW_IF_FAILED(_r.device->CreateTexture2D(&desc, nullptr, _r.glyphScratchpad.put()));
    }

    _setShaderResources();

    // D2D
    {
        D2D1_RENDER_TARGET_PROPERTIES props{};
        props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        props.pixelFormat = { DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED };
        props.dpiX = static_cast<float>(_api.dpi);
        props.dpiY = static_cast<float>(_api.dpi);
        const auto surface = _r.glyphScratchpad.query<IDXGISurface>();
        THROW_IF_FAILED(_sr.d2dFactory->CreateDxgiSurfaceRenderTarget(surface.get(), &props, _r.d2dRenderTarget.put()));
        // We don't really use D2D for anything except DWrite, but it
        // can't hurt to ensure that everything it does is pixel aligned.
        _r.d2dRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        _r.d2dRenderTarget->SetTextAntialiasMode(static_cast<D2D1_TEXT_ANTIALIAS_MODE>(_api.antialiasingMode));
    }
    {
        static constexpr D2D1_COLOR_F color{ 1, 1, 1, 1 };
        wil::com_ptr<ID2D1SolidColorBrush> brush;
        THROW_IF_FAILED(_r.d2dRenderTarget->CreateSolidColorBrush(&color, nullptr, brush.addressof()));
        _r.brush = brush.query<ID2D1Brush>();
    }
    {
        getLocaleName(localeName);
        _r.localeName = { localeName };

        for (auto style = 0; style < 2; ++style)
        {
            for (auto weight = 0; weight < 2; ++weight)
            {
                _r.textFormats[style][weight] = _createTextFormat(
                    _api.fontName.c_str(),
                    weight ? DWRITE_FONT_WEIGHT_BOLD : static_cast<DWRITE_FONT_WEIGHT>(_api.fontWeight),
                    static_cast<DWRITE_FONT_STYLE>(style * DWRITE_FONT_STYLE_ITALIC),
                    _api.fontSize,
                    localeName);
            }
        }
    }

    _drawCursor();

    WI_SetAllFlags(_invalidations, invalidation_flags::cbuffer);
}

void AtlasEngine::_setShaderResources() const
{
    const std::array resources{ _r.cellView.get(), _r.glyphView.get() };
    _r.deviceContext->PSSetShaderResources(0, gsl::narrow_cast<UINT>(resources.size()), resources.data());
}

void AtlasEngine::_updateConstantBuffer() const
{
    const_buffer data;
    data.viewport.x = 0;
    data.viewport.y = 0;
    data.viewport.z = static_cast<float>(_api.cellCount.x * _api.cellSize.x);
    data.viewport.w = static_cast<float>(_api.cellCount.y * _api.cellSize.y);
    data.cellSize.x = _api.cellSize.x;
    data.cellSize.y = _api.cellSize.y;
    data.cellCountX = _api.cellCount.x;
    data.backgroundColor = _rapi.backgroundColor;
    data.selectionColor = _rapi.selectionColor;
    _r.deviceContext->UpdateSubresource(_r.constantBuffer.get(), 0, nullptr, &data, 0, 0);
}

void AtlasEngine::_processBufferLine(u16 y)
{
    TextAnalyzer atlasAnalyzer{ _rapi.bufferLine, _r.localeName.c_str(), _rapi.analysisResults };
    const auto textSize = _rapi.bufferLine.size();
    const auto projectedGlyphSize = 3 * textSize / 2 + 16;

    if (_rapi.clusterMap.size() < textSize)
    {
        _rapi.clusterMap.resize(textSize);
    }
    if (_rapi.textProps.size() < textSize)
    {
        _rapi.textProps.resize(textSize);
    }
    if (_rapi.glyphIndices.size() < projectedGlyphSize)
    {
        _rapi.glyphIndices.resize(projectedGlyphSize);
    }
    if (_rapi.glyphProps.size() < projectedGlyphSize)
    {
        _rapi.glyphProps.resize(projectedGlyphSize);
    }

    const auto textFormat = _getTextFormat(_rapi.attributes.bold, _rapi.attributes.italic);

    for (UINT32 idx = 0, mappedEnd; idx < textSize; idx = mappedEnd)
    {
        std::wstring familyName;
        familyName.resize(textFormat->GetFontFamilyNameLength());
        THROW_IF_FAILED(textFormat->GetFontFamilyName(familyName.data(), static_cast<UINT32>(familyName.size() + 1)));

        wil::com_ptr<IDWriteFontCollection> fontCollection;
        THROW_IF_FAILED(textFormat->GetFontCollection(fontCollection.addressof()));

        UINT32 mappedLength;
        wil::com_ptr<IDWriteFont> mappedFont;
        float scale;
        THROW_IF_FAILED(_sr.systemFontFallback->MapCharacters(
            /* analysisSource     */ &atlasAnalyzer,
            /* textPosition       */ idx,
            /* textLength         */ gsl::narrow_cast<UINT32>(textSize) - idx,
            /* baseFontCollection */ fontCollection.get(),
            /* baseFamilyName     */ familyName.c_str(),
            /* baseWeight         */ _rapi.attributes.bold ? DWRITE_FONT_WEIGHT_BOLD : static_cast<DWRITE_FONT_WEIGHT>(_api.fontWeight),
            /* baseStyle          */ static_cast<DWRITE_FONT_STYLE>(_rapi.attributes.italic * DWRITE_FONT_STYLE_ITALIC),
            /* baseStretch        */ DWRITE_FONT_STRETCH_NORMAL,
            /* mappedLength       */ &mappedLength,
            /* mappedFont         */ mappedFont.addressof(),
            /* scale              */ &scale));
        mappedEnd = idx + mappedLength;

        if (!mappedFont)
        {
            // We can reuse idx here, as it'll be reset to "idx = mappedEnd" in the outer loop anyways.
            auto beg = _rapi.bufferLinePos[idx++];
            for (; idx <= mappedEnd; ++idx)
            {
                const auto cur = _rapi.bufferLinePos[idx];
                if (beg != cur)
                {
                    static constexpr auto replacement = L'\uFFFD';
                    _emplaceGlyph(&replacement, 1, y, beg, cur);
                    beg = cur;
                }
            }

            continue;
        }

        wil::com_ptr<IDWriteFontFace> mappedFontFace;
        THROW_IF_FAILED(mappedFont->CreateFontFace(mappedFontFace.addressof()));

        // We can reuse idx here, as it'll be reset to "idx = mappedEnd" in the outer loop anyways.
        for (UINT32 complexityLength; idx < mappedEnd; idx += complexityLength)
        {
            BOOL isTextSimple;
            THROW_IF_FAILED(_sr.textAnalyzer->GetTextComplexity(_rapi.bufferLine.data() + idx, mappedEnd - idx, mappedFontFace.get(), &isTextSimple, &complexityLength, _rapi.glyphIndices.data()));

            if (isTextSimple)
            {
                for (UINT32 i = 0; i < complexityLength; ++i)
                {
                    _emplaceGlyph(&_rapi.bufferLine[idx + i], 1, y, _rapi.bufferLinePos[idx + i], _rapi.bufferLinePos[idx + i + 1]);
                }
            }
            else
            {
                atlasAnalyzer.Analyze(_sr.textAnalyzer.get(), idx, complexityLength);

                for (const auto& a : _rapi.analysisResults)
                {
                    DWRITE_SCRIPT_ANALYSIS scriptAnalysis{ a.script, static_cast<DWRITE_SCRIPT_SHAPES>(a.shapes) };
                    UINT32 actualGlyphCount;

                    for (auto retry = 0;;)
                    {
                        const auto hr = _sr.textAnalyzer->GetGlyphs(
                            /* textString          */ _rapi.bufferLine.data() + a.textPosition,
                            /* textLength          */ a.textLength,
                            /* fontFace            */ mappedFontFace.get(),
                            /* isSideways          */ false,
                            /* isRightToLeft       */ a.bidiLevel & 1,
                            /* scriptAnalysis      */ &scriptAnalysis,
                            /* localeName          */ _r.localeName.c_str(),
                            /* numberSubstitution  */ nullptr,
                            /* features            */ nullptr,
                            /* featureRangeLengths */ nullptr,
                            /* featureRanges       */ 0,
                            /* maxGlyphCount       */ gsl::narrow_cast<UINT32>(_rapi.glyphProps.size()),
                            /* clusterMap          */ _rapi.clusterMap.data(),
                            /* textProps           */ _rapi.textProps.data(),
                            /* glyphIndices        */ _rapi.glyphIndices.data(),
                            /* glyphProps          */ _rapi.glyphProps.data(),
                            /* actualGlyphCount    */ &actualGlyphCount);

                        if (hr == HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && ++retry < 8)
                        {
                            // grow factor 1.5x
                            auto size = _rapi.glyphProps.size();
                            size = size + (size >> 1);
                            _rapi.glyphIndices.resize(size);
                            _rapi.glyphProps.resize(size);
                            continue;
                        }

                        THROW_IF_FAILED(hr);
                        break;
                    }

                    _rapi.textProps[a.textLength - 1].canBreakShapingAfter = 1;

                    UINT32 beg = 0;
                    for (UINT32 i = 0; i < a.textLength; ++i)
                    {
                        if (_rapi.textProps[i].canBreakShapingAfter)
                        {
                            _emplaceGlyph(&_rapi.bufferLine[a.textPosition + beg], i + 1 - beg, y, _rapi.bufferLinePos[a.textPosition + beg], _rapi.bufferLinePos[a.textPosition + i + 1]);
                            beg = i + 1;
                        }

                        // TextLayout::GetFirstNonzeroWidthGlyph
                        // Requires a call to GetDesignGlyphAdvances() ensuring that the glyphAdvance is >1.
                    }
                }
            }
        }
    }
}

void AtlasEngine::_emplaceGlyph(const wchar_t* key, size_t keyLen, u16 y, u16 x1, u16 x2)
{
    assert(key);
    assert(keyLen != 0);
    assert(y < _api.cellCount.y);
    assert(x1 < _api.cellCount.x);
    assert(x2 <= _api.cellCount.x);
    assert(x1 < x2);

    auto data = _getCell(x1, y);
    u16 cells = x2 - x1;

    if (keyLen > 15)
    {
        keyLen = 15;
    }

    if (cells > 16)
    {
        cells = 16;
    }

    glyph_entry entry{};
    memcpy(&entry.chars[0], key, keyLen * sizeof(wchar_t));
    entry.attributes = _rapi.attributes;
    entry.attributes.cells = cells - 1;

    auto& coords = _r.glyphs[entry];
    if (coords[0] == u16x2{})
    {
        for (u16 i = 0; i < cells; ++i)
        {
            coords[i] = _allocateAtlasCell();
        }
        _r.glyphQueue.emplace_back(entry, coords);
    }

    for (uint32_t i = 0; i < cells; ++i)
    {
        data[i].glyphIndex16 = coords[i];
        data[i].flags = 0;
        data[i].color = _rapi.currentColor;
    }
}

IDWriteTextFormat* AtlasEngine::_getTextFormat(bool bold, bool italic) const noexcept
{
    return _r.textFormats[italic][bold].get();
}

wil::com_ptr<IDWriteTextFormat> AtlasEngine::_createTextFormat(const wchar_t* fontFamilyName, DWRITE_FONT_WEIGHT fontWeight, DWRITE_FONT_STYLE fontStyle, float fontSize, const wchar_t* localeName) const
{
    wil::com_ptr<IDWriteTextFormat> textFormat;
    THROW_IF_FAILED(_sr.dwriteFactory->CreateTextFormat(fontFamilyName, nullptr, fontWeight, fontStyle, DWRITE_FONT_STRETCH_NORMAL, fontSize, localeName, textFormat.addressof()));
    textFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    return textFormat;
}

AtlasEngine::u16x2 AtlasEngine::_allocateAtlasCell() noexcept
{
    const auto ret = _r.atlasPosition;

    _r.atlasPosition.x += _api.cellSize.x;
    if (_r.atlasPosition.x >= _r.atlasSizeInPixel.x)
    {
        _r.atlasPosition.x = 0;
        _r.atlasPosition.y += _api.cellSize.y;
        if (_r.atlasPosition.y >= _r.atlasSizeInPixel.y)
        {
            _r.atlasPosition.x = _api.cellSize.x;
            _r.atlasPosition.y = 0;
        }
    }

    return ret;
}

void AtlasEngine::_drawGlyph(const til::pair<glyph_entry, std::array<u16x2, 16>>& pair) const
{
    const auto entry = pair.first;
    const auto charsLength = wcsnlen_s(&entry.chars[0], std::size(entry.chars));
    const auto cells = entry.attributes.cells + UINT32_C(1);
    const bool bold = entry.attributes.bold;
    const bool italic = entry.attributes.italic;
    const auto textFormat = _getTextFormat(bold, italic);

    D2D1_RECT_F rect;
    rect.left = 0.0f;
    rect.top = 0.0f;
    rect.right = static_cast<float>(cells) * _api.cellSizeDIP.x;
    rect.bottom = _api.cellSizeDIP.y;

    {
        // See D2DFactory::DrawText
        wil::com_ptr<IDWriteTextLayout> textLayout;
        THROW_IF_FAILED(_sr.dwriteFactory->CreateTextLayout(&entry.chars[0], gsl::narrow_cast<UINT32>(charsLength), textFormat, rect.right, rect.bottom, textLayout.addressof()));

        _r.d2dRenderTarget->BeginDraw();
        _r.d2dRenderTarget->Clear();
        _r.d2dRenderTarget->DrawTextLayout({}, textLayout.get(), _r.brush.get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        THROW_IF_FAILED(_r.d2dRenderTarget->EndDraw());
    }

    for (uint32_t i = 0; i < cells; ++i)
    {
        // Specifying NO_OVERWRITE means that the system can assume that existing references to the surface that
        // may be in flight on the GPU will not be affected by the update, so the copy can proceed immediately
        // (avoiding either a batch flush or the system maintaining multiple copies of the resource behind the scenes).
        //
        // Since our shader only draws whatever is in the atlas, and since we don't replace glyph cells that are in use,
        // we can safely (?) tell the GPU that we don't overwrite parts of our atlas that are in use.
        _copyScratchpadCell(i, pair.second[i], D3D11_COPY_NO_OVERWRITE);
    }
}

void AtlasEngine::_drawCursor() const
{
    D2D1_RECT_F rect;
    rect.left = 0;
    rect.top = _api.cellSizeDIP.y * 0.81f;
    rect.right = _api.cellSizeDIP.x;
    rect.bottom = _api.cellSizeDIP.y;

    _r.d2dRenderTarget->BeginDraw();
    _r.d2dRenderTarget->Clear();
    _r.d2dRenderTarget->FillRectangle(&rect, _r.brush.get());
    THROW_IF_FAILED(_r.d2dRenderTarget->EndDraw());

    _copyScratchpadCell(0, {});
}

void AtlasEngine::_copyScratchpadCell(uint32_t scratchpadIndex, u16x2 target, uint32_t copyFlags) const
{
    D3D11_BOX box;
    box.left = scratchpadIndex * _api.cellSize.x;
    box.top = 0;
    box.front = 0;
    box.right = (scratchpadIndex + 1) * _api.cellSize.x;
    box.bottom = _api.cellSize.y;
    box.back = 1;
    _r.deviceContext->CopySubresourceRegion1(_r.glyphBuffer.get(), 0, target.x, target.y, 0, _r.glyphScratchpad.get(), 0, &box, copyFlags);
}
