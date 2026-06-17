enum ButtonPressResult
{
  LongPress,
  DoubleClick,
  TripleClick,
  ShortPress,
  SoftReset,
  NoAction
};
extern const char *ButtonPressResultNames[6];

ButtonPressResult read_button_presses();

ButtonPressResult read_long_press();