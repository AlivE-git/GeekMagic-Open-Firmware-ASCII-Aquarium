function displayHandler() {
    return {
        brightness: 100,
        saved: 100,
        ready: false,
        saving: false,
        status: "",

        init() {
            apiFetch("/api/v1/display/brightness")
                .then((r) => r.json())
                .then((data) => {
                    this.brightness = data.brightness ?? 100;
                    this.saved = this.brightness;
                })
                .catch((err) => console.error("failed to fetch brightness", err))
                .finally(() => {
                    this.ready = true;
                });

            apiFetch("/api/v1/weather/config")
                .then((r) => r.json())
                .then((data) => {
                    this.weatherLocation = data.weather_location || "";
                    this.weatherApiKey = data.weather_api_key || "";
                })
                .catch((err) => console.error("failed to fetch weather config", err));
        },

        // The slider only changes the panel: no flash write, the value is forgotten on reboot.
        setBrightness(value) {
            this.brightness = parseInt(value, 10);
            this.status = "";
            clearTimeout(this._brightnessTimer);
            this._brightnessTimer = setTimeout(() => this.push(), 120);
        },

        push() {
            return apiFetch("/api/v1/display/brightness", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ brightness: this.brightness }),
            }).catch((err) => {
                console.error("failed to set brightness", err);
                this.status = "Not applied";
            });
        },

        // Save writes the value to flash, so it survives a reboot.
        save() {
            this.saving = true;
            this.status = "";

            apiFetch("/api/v1/display/brightness", {
                method: "POST",
                headers: { "Content-Type": "application/json" },
                body: JSON.stringify({ brightness: this.brightness, persist: true }),
            })
                .then((response) => {
                    if (!response.ok) throw new Error("HTTP " + response.status);
                    return response.json();
                })
                .then(() => {
                    this.saved = this.brightness;
                    this.status = "Saved";
                })
                .catch((err) => {
                    console.error("failed to save brightness", err);
                    this.status = "Save failed";
                })
                .finally(() => {
                    this.saving = false;
                });
        },
    };
}
