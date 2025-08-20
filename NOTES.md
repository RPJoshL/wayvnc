## Zum bauen und ausführen

meson build; ninja -C build; ./build/wayvnc 0.0.0.0 5911 --log-level=debug -S /run/user/1000/wayvncDev --render-cursor --gpu
