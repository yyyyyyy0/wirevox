#include "Theme.h"

namespace mfx::ui
{

//==============================================================================
float textWidth (const FontOptions& options, const String& text)
{
    GlyphArrangement glyphs;
    glyphs.addLineOfText (Font (options), text, 0.0f, 0.0f);
    return glyphs.getBoundingBox (0, -1, true).getWidth();
}

String fitText (const FontOptions& options, const String& text, float maxWidth)
{
    if (textWidth (options, text) <= maxWidth)
        return text;

    auto shortened = text;

    while (shortened.isNotEmpty() && textWidth (options, shortened + juce_wchar (0x2026)) > maxWidth)
        shortened = shortened.dropLastCharacters (1);

    return shortened + juce_wchar (0x2026);
}

void drawText (Graphics& g, const String& text, Rectangle<float> area, Justification justification,
               const FontOptions& font, Colour colour)
{
    g.setFont (Font (font));
    g.setColour (colour);
    g.drawText (text, area, justification, false);
}

void drawGlowText (Graphics& g, const String& text, Rectangle<float> area, Justification justification,
                   const FontOptions& font, Colour colour, float glowAlpha)
{
    for (auto offset : { Point<float> (-1.0f, 0.0f), Point<float> (1.0f, 0.0f), Point<float> (0.0f, -1.0f), Point<float> (0.0f, 1.0f) })
        drawText (g, text, area.translated (offset.x, offset.y), justification, font, colour.withMultipliedAlpha (glowAlpha));

    drawText (g, text, area, justification, font, colour);
}

void drawScanlines (Graphics& g, Rectangle<float> area, float spacing, float alpha)
{
    g.setColour (Colours::black.withAlpha (alpha));

    for (auto y = area.getY(); y < area.getBottom(); y += spacing)
        g.fillRect (area.getX(), y, area.getWidth(), 1.0f);
}

//==============================================================================
BodyText::BodyText (const String& text, const FontOptions& font, Colour colour, float lineSpacing, float width, float columnGap)
    : gap (columnGap)
{
    const auto lines = StringArray::fromLines (text);
    const auto numColumns = width >= 640.0f && lines.size() >= 6 ? 2 : 1;
    const auto perColumn = (lines.size() + numColumns - 1) / numColumns;
    columnWidth = (width - gap * (float) (numColumns - 1)) / (float) numColumns;

    for (int c = 0; c < numColumns; ++c)
    {
        StringArray part;

        for (int i = c * perColumn; i < jmin (lines.size(), (c + 1) * perColumn); ++i)
            part.add (lines[i]);

        AttributedString attributed;
        attributed.setLineSpacing (lineSpacing);
        attributed.setWordWrap (AttributedString::byWord);
        attributed.append (part.joinIntoString ("\n"), font, colour);

        TextLayout layout;
        layout.createLayout (attributed, jmax (1.0f, columnWidth));
        height = jmax (height, layout.getHeight());
        columns.push_back (std::move (layout));
    }
}

void BodyText::draw (Graphics& g, Rectangle<float> area) const
{
    for (size_t i = 0; i < columns.size(); ++i)
        columns[i].draw (g, Rectangle<float> (area.getX() + (float) i * (columnWidth + gap), area.getY(), columnWidth, area.getHeight()));
}

//==============================================================================
Rectangle<int> Theme::getSelectorBounds (Rectangle<int> strip) const
{
    return { strip.getRight() - 170, strip.getY() + 2, 170, 24 };
}

void Theme::paintComboArrow (Graphics& g, Rectangle<float> key, Colour colour, bool enabled)
{
    const auto centre = key.getCentre();
    Path arrows;
    arrows.addTriangle (centre.x - 3.5f, centre.y - 1.5f, centre.x + 3.5f, centre.y - 1.5f, centre.x, centre.y - 5.0f);
    arrows.addTriangle (centre.x - 3.5f, centre.y + 1.5f, centre.x + 3.5f, centre.y + 1.5f, centre.x, centre.y + 5.0f);

    g.setColour (colour.withMultipliedAlpha (enabled ? 1.0f : 0.4f));
    g.fillPath (arrows);
}

void Theme::paintBypass (Graphics& g, ToggleButton& button, bool hover, bool down)
{
    const ControlState state { button.isEnabled(), hover, down, button.getToggleState() };
    const auto face = button.findColour (state.toggledOn ? TextButton::buttonOnColourId : TextButton::buttonColourId);

    paintControlFace (g, button.getLocalBounds().toFloat().reduced (0.5f), face, state);
    paintKeyLabel (g, button, state);
}

Colour Theme::getButtonTextColour (const Button& button, const ControlState& state) const
{
    return button.findColour (state.toggledOn ? TextButton::textColourOnId : TextButton::textColourOffId);
}

void Theme::paintKeyLabel (Graphics& g, Button& button, const ControlState& state)
{
    g.setFont (Font (getUiFont (jmin (13.0f, (float) button.getHeight() * 0.5f), true)));
    g.setColour (getButtonTextColour (button, state).withMultipliedAlpha (state.enabled ? 1.0f : 0.45f));

    const auto area = button.getLocalBounds().reduced (3, 2).translated (0, state.pressed() ? getPressedTextShift() : 0);
    g.drawFittedText (decorateButtonText (button.getButtonText(), state.toggledOn), area, Justification::centred, 1);
}

Font Theme::getTextButtonFont (TextButton&, int buttonHeight)
{
    return Font (getUiFont (jmin (13.0f, (float) buttonHeight * 0.5f), true));
}

void Theme::drawButtonBackground (Graphics& g, Button& button, const Colour& background, bool highlighted, bool down)
{
    const ControlState state { button.isEnabled(), highlighted, down, button.getToggleState() };
    paintControlFace (g, button.getLocalBounds().toFloat().reduced (0.5f), background, state);
}

void Theme::drawButtonText (Graphics& g, TextButton& button, bool highlighted, bool down)
{
    paintKeyLabel (g, button, { button.isEnabled(), highlighted, down, button.getToggleState() });
}

void Theme::drawToggleButton (Graphics& g, ToggleButton& button, bool highlighted, bool down)
{
    if (dynamic_cast<BypassToggle*> (&button) != nullptr)
    {
        paintBypass (g, button, highlighted, down);
        return;
    }

    const auto fontSize = jmin (15.0f, (float) button.getHeight() * 0.75f);
    const auto tick = fontSize * 1.1f;

    paintTickBox (g, { 6.0f, ((float) button.getHeight() - tick) * 0.5f, tick, tick },
                  button.getToggleState(), button.isEnabled(), highlighted);

    g.setFont (Font (getUiFont (fontSize, false)));
    g.setColour (button.findColour (ToggleButton::textColourId).withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));
    g.drawFittedText (button.getButtonText(), button.getLocalBounds().withTrimmedLeft ((int) (tick + 14.0f)).withTrimmedRight (2),
                      Justification::centredLeft, 10);
}

void Theme::drawTickBox (Graphics& g, Component&, float x, float y, float w, float h,
                         bool ticked, bool isEnabled, bool highlighted, bool)
{
    paintTickBox (g, { x, y, w, h }, ticked, isEnabled, highlighted);
}

void Theme::drawComboBox (Graphics& g, int width, int height, bool isButtonDown,
                          int buttonX, int, int buttonW, int, ComboBox& box)
{
    paintFieldFace (g, Rectangle<float> ((float) width, (float) height), box.isEnabled());

    const auto key = Rectangle<float> ((float) buttonX, 0.0f, (float) buttonW, (float) height).reduced (3.0f);
    paintControlFace (g, key, box.findColour (ComboBox::buttonColourId), { box.isEnabled(), false, isButtonDown, false });
    paintComboArrow (g, key.translated (0.0f, isButtonDown ? 1.0f : 0.0f), box.findColour (ComboBox::arrowColourId), box.isEnabled());
}

Font Theme::getComboBoxFont (ComboBox& box)
{
    return Font (getUiFont (jmin (14.0f, (float) box.getHeight() * 0.65f), false));
}

void Theme::positionComboBoxText (ComboBox& box, Label& label)
{
    label.setBounds (4, 1, box.getWidth() - 4 - 24, box.getHeight() - 2);
    label.setFont (getComboBoxFont (box));
}

Font Theme::getPopupMenuFont()
{
    return Font (getUiFont (14.0f, false));
}

int Theme::getDefaultScrollbarWidth()
{
    return getMetrics().scrollbarWidth;
}

void Theme::drawScrollbar (Graphics& g, ScrollBar& bar, int x, int y, int width, int height,
                           bool isScrollbarVertical, int thumbStartPosition, int thumbSize, bool, bool isMouseDown)
{
    paintFieldFace (g, Rectangle<float> ((float) x, (float) y, (float) width, (float) height).reduced (1.0f), true);

    if (thumbSize <= 0)
        return;

    const auto thumb = isScrollbarVertical
        ? Rectangle<float> ((float) x + 2.0f, (float) (y + thumbStartPosition), (float) width - 4.0f, (float) thumbSize)
        : Rectangle<float> ((float) (x + thumbStartPosition), (float) y + 2.0f, (float) thumbSize, (float) height - 4.0f);

    paintControlFace (g, thumb, bar.findColour (ScrollBar::thumbColourId), { true, false, isMouseDown, false });
}

void Theme::applyScheme (const Scheme& s)
{
    setColour (ResizableWindow::backgroundColourId, s.window);
    setColour (DocumentWindow::textColourId, s.text);

    setColour (Label::textColourId, s.text);
    setColour (Label::backgroundColourId, Colours::transparentBlack);
    setColour (Label::outlineColourId, Colours::transparentBlack);

    setColour (TextButton::buttonColourId, s.face);
    setColour (TextButton::buttonOnColourId, s.faceOn);
    setColour (TextButton::textColourOffId, s.buttonText);
    setColour (TextButton::textColourOnId, s.buttonTextOn);

    setColour (ToggleButton::textColourId, s.text);
    setColour (ToggleButton::tickColourId, s.accent);
    setColour (ToggleButton::tickDisabledColourId, s.textDim.withAlpha (0.4f));

    setColour (ComboBox::backgroundColourId, s.field);
    setColour (ComboBox::textColourId, s.text);
    setColour (ComboBox::outlineColourId, s.textDim);
    setColour (ComboBox::buttonColourId, s.face);
    setColour (ComboBox::arrowColourId, s.accent);
    setColour (ComboBox::focusedOutlineColourId, s.accent);

    setColour (PopupMenu::backgroundColourId, s.field);
    setColour (PopupMenu::textColourId, s.text);
    setColour (PopupMenu::headerTextColourId, s.textDim);
    setColour (PopupMenu::highlightedBackgroundColourId, s.accent);
    setColour (PopupMenu::highlightedTextColourId, s.onAccent);

    setColour (ScrollBar::thumbColourId, s.face);
    setColour (ScrollBar::backgroundColourId, s.field);

    setColour (ListBox::backgroundColourId, Colours::transparentBlack);
    setColour (ListBox::outlineColourId, Colours::transparentBlack);
    setColour (ListBox::textColourId, s.text);

    setColour (TextEditor::backgroundColourId, s.field);
    setColour (TextEditor::textColourId, s.text);
    setColour (TextEditor::outlineColourId, s.textDim);
    setColour (TextEditor::focusedOutlineColourId, s.accent);
    setColour (TextEditor::highlightColourId, s.accent.withAlpha (0.5f));

    setColour (AlertWindow::backgroundColourId, s.window);
    setColour (AlertWindow::textColourId, s.text);
    setColour (AlertWindow::outlineColourId, s.textDim);

    setColour (Slider::textBoxTextColourId, s.text);
    setColour (Slider::textBoxBackgroundColourId, s.field);
    setColour (Slider::textBoxOutlineColourId, s.textDim);
    setColour (Slider::thumbColourId, s.accent);
    setColour (Slider::trackColourId, s.accent);
    setColour (Slider::backgroundColourId, s.field);
}

Theme* findTheme (Component& component)
{
    return dynamic_cast<Theme*> (&component.getLookAndFeel());
}

//==============================================================================
void ReadoutPanel::setContent (const ReadoutContent& newContent)
{
    if (newContent.caption == content.caption && newContent.bigValue == content.bigValue
        && newContent.unit == content.unit && newContent.body == content.body)
        return;

    content = newContent;
    repaint();
}

int ReadoutPanel::getPreferredHeight (int width)
{
    auto* theme = findTheme (*this);
    return theme != nullptr ? theme->measureReadout (width, content) : 100;
}

void ReadoutPanel::paint (Graphics& g)
{
    if (auto* theme = findTheme (*this))
        theme->paintReadout (g, getLocalBounds(), content);
}

void StatusStrip::setText (const String& newText)
{
    if (newText == text)
        return;

    text = newText;
    repaint();
}

void StatusStrip::paint (Graphics& g)
{
    if (auto* theme = findTheme (*this))
        theme->paintStatus (g, getLocalBounds(), text);
}

} // namespace mfx::ui
