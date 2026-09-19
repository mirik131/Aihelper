#pragma once

// Плавности для гуи. Fade: значение ползёт к цели со скоростью speed.
// Каждый кадр: fade.target = ...; fade.update(dt); взять fade.value (0..1).

struct Fade {
    float value = 0.0f;  // сейчас
    float target = 0.0f; // куда идём
    float speed = 6.0f;  // как быстро

    void update(float dt) {
        if (value < target) {
            value += speed * dt;
            if (value > target)
                value = target;
        } else if (value > target) {
            value -= speed * dt;
            if (value < target)
                value = target;
        }
    }

    bool ready() const { return value == target; }
};
