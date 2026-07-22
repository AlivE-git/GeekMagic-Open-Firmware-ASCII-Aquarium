function displayHandler() {
    return {
        brightness: 100,
        loading: false,

        init() {
            apiFetch("/api/v1/display/brightness")
                .then((r) => r.json())
                .then((data) => {
                    this.brightness = data.brightness ?? 100;
                })
                .catch((err) => console.error("failed to fetch brightness", err));

            apiFetch("/api/v1/weather/config")
                .then((r) => r.json())
                .then((data) => {
                    this.weatherLocation = data.weather_location || "";
                    this.weatherApiKey = data.weather_api_key || "";
                })
                .catch((err) => console.error("failed to fetch weather config", err));
        },

        setBrightness(value) {
            this.brightness = parseInt(value);
            clearTimeout(this._brightnessTimer);
            this._brightnessTimer = setTimeout(() => {
                apiFetch("/api/v1/display/brightness", {
                    method: "POST",
                    headers: { "Content-Type": "application/json" },
                    body: JSON.stringify({ brightness: this.brightness }),
                }).catch((err) => console.error("failed to set brightness", err));
            }, 150);
        },
    };
}