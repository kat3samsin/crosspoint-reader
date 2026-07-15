#pragma once

class MappedInputManager {
 public:
  enum class Button { Back, Confirm, Left, Right, Up, Down };

  struct Labels {
    const char* btn1;
    const char* btn2;
    const char* btn3;
    const char* btn4;
  };

  void setPressed(Button button) {
    pressed = button;
    hasPressed = true;
    hasReleased = false;
  }

  void setReleased(Button button) {
    released = button;
    hasReleased = true;
    hasPressed = false;
  }

  bool wasPressed(Button button) const { return hasPressed && pressed == button; }
  bool wasReleased(Button button) const { return hasReleased && released == button; }

  Labels mapLabels(const char* back, const char* confirm, const char* previous, const char* next) const {
    return {back, confirm, previous, next};
  }

 private:
  Button pressed = Button::Back;
  Button released = Button::Back;
  bool hasPressed = false;
  bool hasReleased = false;
};
