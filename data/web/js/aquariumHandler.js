function rgb565ToHex(color) {
  const r = (color >> 11) & 0x1f;
  const g = (color >> 5) & 0x3f;
  const b = color & 0x1f;

  const hex = (value) => value.toString(16).padStart(2, "0");

  return (
    "#" +
    hex(Math.round((r * 255) / 31)) +
    hex(Math.round((g * 255) / 63)) +
    hex(Math.round((b * 255) / 31))
  );
}

function hexToRgb565(hex) {
  const value = parseInt(hex.replace("#", ""), 16);
  const r = (value >> 16) & 0xff;
  const g = (value >> 8) & 0xff;
  const b = value & 0xff;

  return (
    ((Math.round((r * 31) / 255) & 0x1f) << 11) |
    ((Math.round((g * 63) / 255) & 0x3f) << 5) |
    (Math.round((b * 31) / 255) & 0x1f)
  );
}

function aquariumHandler() {
  return {
    cfg: {
      arc_height: 27,
      arc_offset: 9,
      arc_bulge: 5,
      arc_stroke: 1,
      stagger: 3,
      sway_max: 4,
      sway_phase_step: 0,
      sway_step_ms: 60,
      color: 2016,
      fish_count: 3,
      fish_scale_min: 2,
      fish_scale_max: 3,
      fish_speed_min: 15,
      fish_speed_max: 45,
      fish_bob_min: 1,
      fish_bob_max: 3,
      fish_turn_anim: true,
      fish_swim: "free",
      bubbles: true,
      bubble_chance: 20,
      bubble_rise: 10,
      bubble_every_ms: 1200,
      weed_grow_s: 20,
      weed_eat: true,
      fish_eat_chance: 30,
      weeds: [
        { x: 72, brackets: 2 },
        { x: 168, brackets: 3 },
      ],
    },
    colorHex: "#07e0",
    statusMsg: "",
    loading: false,

    init() {
      this.load();
    },

    load() {
      apiFetch("/api/v1/aquarium")
        .then((response) => response.json())
        .then((data) => {
          if (Array.isArray(data.weeds)) {
            this.cfg = data;
          } else {
            this.cfg = { ...this.cfg, ...data, weeds: this.cfg.weeds };
          }
          this.colorHex = rgb565ToHex(this.cfg.color);
        })
        .catch((err) => {
          console.error("failed to fetch aquarium config", err);
          this.statusMsg = "Could not load settings";
        });
    },

    // Applied live while dragging, without touching the flash.
    push() {
      clearTimeout(this._applyTimer);
      this._applyTimer = setTimeout(() => this.send(false), 120);
    },

    save() {
      this.send(true);
    },

    send(persist) {
      this.loading = true;
      const body = JSON.stringify({ ...this.cfg, color: this.cfg.color, persist });

      apiFetch("/api/v1/aquarium", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body,
      })
        .then((response) => response.json().then((data) => ({ ok: response.ok, data })))
        .then(({ ok, data }) => {
          this.statusMsg = ok
            ? persist
              ? "Saved, survives reboot"
              : "Applied"
            : "Error: " + (data.message || "invalid settings");
        })
        .catch((err) => {
          console.error("failed to apply aquarium config", err);
          this.statusMsg = "Could not apply settings";
        })
        .finally(() => {
          this.loading = false;
        });
    },

    onColor(event) {
      this.colorHex = event.target.value;
      this.cfg.color = hexToRgb565(event.target.value);
      this.push();
    },

    // Adding or removing seaweed: the device draws exactly the array it is sent.
    setWeedCount(value) {
      const count = Math.max(0, Math.min(5, Number(value) || 0));
      const weeds = this.cfg.weeds.slice(0, count);

      const defaultX = [72, 168, 120, 48, 192];

      while (weeds.length < count) {
        weeds.push({ x: defaultX[weeds.length] || 120, brackets: 2 });
      }

      this.cfg.weeds = weeds;
      this.push();
    },

    onSlidersChanged() {
      this.push();
    },
  };
}
