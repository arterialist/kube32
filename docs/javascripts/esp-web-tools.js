/* Browser installer (flash.md): loads ESP Web Tools on pages with an
   <esp-web-install-button> and shows the version its manifest offers, or a
   note when the site has no firmware yet. Runs again after each instant
   navigation. */

const ESP_WEB_TOOLS = "https://unpkg.com/esp-web-tools@10.4.0/dist/web/install-button.js?module";

function setupInstaller() {
  const button = document.querySelector("esp-web-install-button");
  if (!button) {
    return;
  }
  import(ESP_WEB_TOOLS);
  fetch(button.getAttribute("manifest"), { cache: "no-cache" })
    .then(response => (response.ok ? response.json() : Promise.reject(response.status)))
    .then(manifest => {
      document.querySelectorAll("[data-firmware-version]").forEach(el => {
        el.textContent = manifest.version;
      });
    })
    .catch(() => {
      button.hidden = true;
      document.querySelectorAll("[data-firmware-missing]").forEach(el => {
        el.hidden = false;
      });
    });
}

if (typeof document$ !== "undefined") {
  document$.subscribe(setupInstaller);
} else {
  document.addEventListener("DOMContentLoaded", setupInstaller);
}
