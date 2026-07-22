#include "PluginEditor.h"

D110AudioProcessorEditor::D110AudioProcessorEditor(D110AudioProcessor &processor)
	: AudioProcessorEditor(&processor), audioProcessor(processor) {
	setSize(1650, 434);

	brandLabel.setFont(juce::Font(22.0f, juce::Font::bold));
	brandLabel.setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
	addAndMakeVisible(brandLabel);

	modelNumberLabel.setFont(juce::Font(22.0f, juce::Font::bold));
	modelNumberLabel.setColour(juce::Label::textColourId, juce::Colour(kAccentBlue));
	addAndMakeVisible(modelNumberLabel);

	modelLabel.setFont(juce::Font(11.0f));
	modelLabel.setColour(juce::Label::textColourId, juce::Colour(kAccentBlue));
	addAndMakeVisible(modelLabel);

	// EXIT/PATCH/.../ENTER captions, PART, PARAMETER, VALUE, MEMORY CARD, MIDI MESSAGE and POWER
	// are all white on the real unit's panel.
	for (auto *label : {&phonesLabel, &volumeLabel, &volumeMinLabel, &volumeMaxLabel, &partRockerLabel,
						 &parameterSharedLabel, &valueTopLabel,
						 &memoryCardLabel, &midiMessageLabel, &powerLabel}) {
		label->setFont(juce::Font(10.0f));
		label->setJustificationType(juce::Justification::centred);
		label->setColour(juce::Label::textColourId, juce::Colours::whitesmoke);
		addAndMakeVisible(*label);
	}
	// Only the second word - GROUP, BANK, NUMBER - is blue.
	for (auto *label : {&paramGroupBottomLabel, &paramBankBottomLabel, &valueBottomLabel}) {
		label->setFont(juce::Font(10.0f));
		label->setJustificationType(juce::Justification::centred);
		label->setColour(juce::Label::textColourId, juce::Colour(kAccentBlue));
		addAndMakeVisible(*label);
	}

	volumeKnob.setSliderStyle(juce::Slider::RotaryHorizontalVerticalDrag);
	volumeKnob.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
	volumeKnob.setColour(juce::Slider::rotarySliderFillColourId, juce::Colours::whitesmoke);
	volumeKnob.setColour(juce::Slider::rotarySliderOutlineColourId, juce::Colour(0xff444448));
	volumeKnob.setColour(juce::Slider::thumbColourId, juce::Colours::whitesmoke);
	addAndMakeVisible(volumeKnob);
	masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment>(
		audioProcessor.parameters, "masterVolume", volumeKnob);

	addAndMakeVisible(lcd);

	exitButton.onClick = [this] { audioProcessor.resetDisplayToMainMode(); };
	addAndMakeVisible(exitButton);
	addAndMakeVisible(exitCaption);

	for (const auto &entry : {std::pair<PanelButton *, const char *>(&patchButton, "PATCH"),
							   {&timbreButton, "TIMBRE"}, {&writeCopyButton, "WRITE/COPY"},
							   {&editButton, "EDIT"}, {&partButton, "PART"}, {&systemButton, "SYSTEM"},
							   {&enterButton, "ENTER"}}) {
		juce::String name(entry.second);
		entry.first->onClick = [this, name] { stubButtonPressed(name); };
		addAndMakeVisible(*entry.first);
	}
	for (auto *caption : {&patchCaption, &timbreCaption, &writeCopyCaption,
						   &editCaption, &partCaption, &systemCaption, &enterCaption})
		addAndMakeVisible(*caption);

	for (const auto &entry : {std::pair<PanelButton *, const char *>(&paramGroupUp, "PARAMETER/GROUP +"),
							   {&paramGroupDown, "PARAMETER/GROUP -"}, {&paramBankUp, "PARAMETER/BANK +"},
							   {&paramBankDown, "PARAMETER/BANK -"}}) {
		juce::String name(entry.second);
		entry.first->onClick = [this, name] { stubButtonPressed(name); };
		addAndMakeVisible(*entry.first);
	}

	// PART steps which Part is being browsed; VALUE/NUMBER steps that Part's Patch up/down -
	// matching the real D-110's PART + VALUE/NUMBER patch-browsing workflow.
	partUp.onClick = [this] { audioProcessor.selectNextPart(); };
	partDown.onClick = [this] { audioProcessor.selectPreviousPart(); };
	valueUp.onClick = [this] { audioProcessor.stepPatch(1); };
	valueDown.onClick = [this] { audioProcessor.stepPatch(-1); };
	for (auto *button : {&partUp, &partDown, &valueUp, &valueDown}) addAndMakeVisible(*button);

	addAndMakeVisible(midiLed);

	powerSwitch.setClickingTogglesState(true);
	powerSwitch.setToggleState(true, juce::dontSendNotification);
	powerSwitch.onClick = [this] { audioProcessor.setPoweredOn(powerSwitch.getToggleState()); };
	addAndMakeVisible(powerSwitch);

	utilitiesHeader.setFont(juce::Font(13.0f, juce::Font::bold));
	utilitiesHeader.setColour(juce::Label::textColourId, juce::Colours::darkgrey);
	addAndMakeVisible(utilitiesHeader);

	addAndMakeVisible(importBankButton);
	importBankButton.onClick = [this] { chooseSysexBank(); };

	importStatusLabel.setJustificationType(juce::Justification::topLeft);
	importStatusLabel.setColour(juce::Label::textColourId, juce::Colours::lightblue);
	addAndMakeVisible(importStatusLabel);

	actionFeedbackLabel.setJustificationType(juce::Justification::topLeft);
	actionFeedbackLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
	actionFeedbackLabel.setFont(juce::Font(12.0f, juce::Font::italic));
	addAndMakeVisible(actionFeedbackLabel);

	midiChannelHintLabel.setText(
		"Note: by default Part 1 listens on MIDI channel 2 (not 1) - matches real hardware.",
		juce::dontSendNotification);
	midiChannelHintLabel.setJustificationType(juce::Justification::topLeft);
	midiChannelHintLabel.setFont(juce::Font(13.0f, juce::Font::italic));
	addAndMakeVisible(midiChannelHintLabel);

	addAndMakeVisible(reverbToggle);
	reverbAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
		audioProcessor.parameters, "reverbEnabled", reverbToggle);

	addAndMakeVisible(superModeToggle);
	superModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment>(
		audioProcessor.parameters, "superMode", superModeToggle);

	refreshLcdAndLed();
	startTimerHz(15);
}

D110AudioProcessorEditor::~D110AudioProcessorEditor() {
	stopTimer();
}

void D110AudioProcessorEditor::paint(juce::Graphics &g) {
	g.fillAll(juce::Colour(0xff202024));

	auto rackArea = getLocalBounds().removeFromTop(kRackHeight);
	g.setColour(juce::Colour(0xff2b2b30));
	g.fillRect(rackArea);
	g.setColour(juce::Colours::black);
	g.drawLine(0.0f, static_cast<float>(kRackHeight), static_cast<float>(getWidth()),
			   static_cast<float>(kRackHeight), 2.0f);

	// Headphone jack (decorative - no functional headphone-only output in a plugin).
	g.setColour(juce::Colour(0xff111114));
	g.fillEllipse(phonesJackBounds.toFloat());
	g.setColour(juce::Colour(0xff444448));
	g.drawEllipse(phonesJackBounds.toFloat(), 2.0f);

	// Memory Card slot (decorative - no real memory card support in the plugin).
	g.setColour(juce::Colour(0xff111114));
	g.fillRoundedRectangle(memoryCardSlotBounds.toFloat(), 3.0f);
	g.setColour(juce::Colour(0xff444448));
	g.drawRoundedRectangle(memoryCardSlotBounds.toFloat(), 3.0f, 1.0f);
}

void D110AudioProcessorEditor::resized() {
	auto area = getLocalBounds();
	auto rackArea = area.removeFromTop(kRackHeight).reduced(14, 10);

	// The logo only sits above Phones/Volume in the upper-left corner on the real unit - it does
	// NOT span the full panel width, and everything else (LCD, buttons, memory card, MIDI/power)
	// uses the FULL rack height independently of it, starting from the very top of the strip.
	auto leftColumn = rackArea.removeFromLeft(250);
	auto logoArea = leftColumn.removeFromTop(56);
	auto brandRow = logoArea.removeFromTop(30);
	brandLabel.setBounds(brandRow.removeFromLeft(90));
	modelNumberLabel.setBounds(brandRow);
	modelLabel.setBounds(logoArea);

	auto phonesVolumeArea = leftColumn;
	auto phonesArea = phonesVolumeArea.removeFromLeft(phonesVolumeArea.getWidth() / 2);
	phonesLabel.setBounds(phonesArea.removeFromTop(16));
	phonesJackBounds = phonesArea.withSizeKeepingCentre(32, 32);

	auto volArea = phonesVolumeArea;
	volumeLabel.setBounds(volArea.removeFromTop(16));
	auto volMinMaxArea = volArea.removeFromBottom(14);
	volumeKnob.setBounds(volArea.withSizeKeepingCentre(58, 58));
	volumeMinLabel.setBounds(volMinMaxArea.removeFromLeft(volMinMaxArea.getWidth() / 2));
	volumeMaxLabel.setBounds(volMinMaxArea);

	rackArea.removeFromLeft(12);

	auto mainRow = rackArea; // full rack height - independent of the logo above Phones/Volume

	// The real LCD module is a wide, short rectangle (roughly 3:1) - not a near-square panel.
	auto lcdArea = mainRow.removeFromLeft(380).reduced(0, 25);
	lcd.setBounds(lcdArea);

	mainRow.removeFromLeft(12);

	// The real D-110 has ONE continuous strip of 8 flat buttons on top and 8 on the bottom -
	// EXIT/PATCH/TIMBRE/[4 up-arrows]/WRITE-COPY, then EDIT/PART/SYSTEM/[4 down-arrows]/ENTER.
	// Columns 1-3 and 8 caption their own button above (row 1) or below (row 2); the 4 arrow
	// columns instead share one caption row sandwiched BETWEEN row 1 and row 2 (PART/GROUP/BANK/NUMBER).
	// The 4 arrow columns need a TWO-line mid caption (e.g. white "PARAMETER" above blue "GROUP"),
	// except the PART column which only has one word - passed as midBottom with midTop left null.
	auto layoutColumn = [](juce::Rectangle<int> col, juce::Label *topCaption, juce::Button &topButton,
							juce::Label *midTop, juce::Label *midBottom, juce::Button &bottomButton,
							juce::Label *bottomCaption) {
		col = col.withSizeKeepingCentre(col.getWidth(), 132); // vertically centre the 5-row stack
		auto topCaptionArea = col.removeFromTop(14);
		if (topCaption != nullptr) topCaption->setBounds(topCaptionArea);
		topButton.setBounds(col.removeFromTop(40).reduced(4, 3));
		auto midArea = col.removeFromTop(24);
		if (midTop != nullptr) midTop->setBounds(midArea.removeFromTop(12));
		if (midBottom != nullptr) midBottom->setBounds(midArea);
		bottomButton.setBounds(col.removeFromTop(40).reduced(4, 3));
		auto bottomCaptionArea = col.removeFromTop(14);
		if (bottomCaption != nullptr) bottomCaption->setBounds(bottomCaptionArea);
	};

	const int colWidth = 72;
	const int colGap = 6;
	auto gridArea = mainRow.removeFromLeft(8 * colWidth + 7 * colGap);
	auto nextGridColumn = [&] {
		auto col = gridArea.removeFromLeft(colWidth);
		gridArea.removeFromLeft(colGap);
		return col;
	};

	layoutColumn(nextGridColumn(), &exitCaption, exitButton, nullptr, nullptr, editButton, &editCaption);
	layoutColumn(nextGridColumn(), &patchCaption, patchButton, nullptr, nullptr, partButton, &partCaption);
	layoutColumn(nextGridColumn(), &timbreCaption, timbreButton, nullptr, nullptr, systemButton, &systemCaption);
	layoutColumn(nextGridColumn(), nullptr, partUp, nullptr, &partRockerLabel, partDown, nullptr);

	// GROUP and BANK share a single "PARAMETER" label centred over both of their columns, rather
	// than repeating it above each one - capture the two column rects to compute that shared span.
	auto groupCol = nextGridColumn();
	auto bankCol = nextGridColumn();
	layoutColumn(groupCol, nullptr, paramGroupUp, nullptr, &paramGroupBottomLabel, paramGroupDown, nullptr);
	layoutColumn(bankCol, nullptr, paramBankUp, nullptr, &paramBankBottomLabel, paramBankDown, nullptr);
	{
		auto sharedArea = groupCol.getUnion(bankCol).withSizeKeepingCentre(groupCol.getWidth() * 2 + colGap, 132);
		sharedArea.removeFromTop(14 + 40); // skip past the (empty) top-caption row and the up-arrow row
		parameterSharedLabel.setBounds(sharedArea.removeFromTop(12));
	}

	layoutColumn(nextGridColumn(), nullptr, valueUp, &valueTopLabel, &valueBottomLabel, valueDown, nullptr);
	layoutColumn(nextGridColumn(), &writeCopyCaption, writeCopyButton, nullptr, nullptr, enterButton, &enterCaption);

	mainRow.removeFromLeft(12);

	// Memory Card label sits directly above its slot, not floating at the top of the whole column.
	auto cardArea = mainRow.removeFromLeft(150);
	auto cardUnit = cardArea.withSizeKeepingCentre(cardArea.getWidth(), 40);
	memoryCardLabel.setBounds(cardUnit.removeFromTop(16));
	memoryCardSlotBounds = cardUnit.withSizeKeepingCentre(cardUnit.getWidth() - 10, 20);

	mainRow.removeFromLeft(12);

	auto rightCluster = mainRow;
	auto midiUnit = rightCluster.removeFromTop(rightCluster.getHeight() / 2).withSizeKeepingCentre(
		rightCluster.getWidth(), 40);
	midiMessageLabel.setBounds(midiUnit.removeFromTop(16));
	midiLed.setBounds(midiUnit.withSizeKeepingCentre(14, 14));

	auto powerUnit = rightCluster.withSizeKeepingCentre(rightCluster.getWidth(), 56);
	powerLabel.setBounds(powerUnit.removeFromTop(16));
	powerSwitch.setBounds(powerUnit.withSizeKeepingCentre(36, 36));

	// --- utilities strip ---
	auto utilArea = area.reduced(12);
	utilitiesHeader.setBounds(utilArea.removeFromTop(20));
	utilArea.removeFromTop(4);

	auto importRow = utilArea.removeFromTop(28);
	importBankButton.setBounds(importRow.removeFromLeft(220));
	importRow.removeFromLeft(10);
	importStatusLabel.setBounds(importRow);

	utilArea.removeFromTop(4);
	actionFeedbackLabel.setBounds(utilArea.removeFromTop(18));

	utilArea.removeFromTop(6);
	midiChannelHintLabel.setBounds(utilArea.removeFromTop(20));

	utilArea.removeFromTop(6);
	reverbToggle.setBounds(utilArea.removeFromTop(24));
	utilArea.removeFromTop(4);
	superModeToggle.setBounds(utilArea.removeFromTop(24));
}

void D110AudioProcessorEditor::timerCallback() {
	refreshLcdAndLed();
	importStatusLabel.setText(audioProcessor.getLastImportMessage(), juce::dontSendNotification);
}

void D110AudioProcessorEditor::refreshLcdAndLed() {
	auto snapshot = audioProcessor.getLcdSnapshot();
	lcd.setSnapshot(snapshot);
	midiLed.setOn(snapshot.midiLedOn);
}

void D110AudioProcessorEditor::stubButtonPressed(const juce::String &buttonName) {
	actionFeedbackLabel.setText(buttonName + ": not implemented yet", juce::dontSendNotification);
}

void D110AudioProcessorEditor::chooseSysexBank() {
	fileChooser = std::make_unique<juce::FileChooser>(
		"Select a SysEx bank or MIDI file", juce::File(), "*.syx;*.mid;*.smf");
	fileChooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
		[this](const juce::FileChooser &fc) {
			auto file = fc.getResult();
			if (file == juce::File()) return;
			audioProcessor.importSysexBank(file);
		});
}
