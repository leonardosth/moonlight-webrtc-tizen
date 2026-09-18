(function () {
  "use strict";

  const TARGET_APP_ID = "MlWrtcTst1.MoonlightWebRTCTest";
  const statusElement = document.getElementById("status-message");
  const spinnerElement = document.getElementById("spinner");

  function launchMoonlight() {
    if (!window.tizen || !tizen.application) {
      if (statusElement) {
        statusElement.textContent = "Ambiente Tizen não detectado (executando no navegador).";
      }
      return;
    }

    try {
      const appControlData = new tizen.ApplicationControlData("autostart", ["Steam"]);
      const appControl = new tizen.ApplicationControl(
        "http://tizen.org/appcontrol/operation/view",
        null,
        null,
        null,
        [appControlData]
      );

      tizen.application.launchAppControl(
        appControl,
        TARGET_APP_ID,
        function () {
          // Moonlight launched successfully; exit this launcher app
          setTimeout(function () {
            try {
              tizen.application.getCurrentApplication().exit();
            } catch (_e) {}
          }, 300);
        },
        function (error) {
          console.error("Falha ao abrir Moonlight WebRTC:", error);
          if (spinnerElement) {
            spinnerElement.style.display = "none";
          }
          if (statusElement) {
            statusElement.className = "status-message error-message";
            statusElement.textContent = "Moonlight WebRTC Client não encontrado. Por favor, instale o aplicativo Moonlight WebRTC na TV.";
          }
        }
      );
    } catch (err) {
      console.error("Erro ao preparar AppControl:", err);
      if (spinnerElement) {
        spinnerElement.style.display = "none";
      }
      if (statusElement) {
        statusElement.className = "status-message error-message";
        statusElement.textContent = "Erro ao iniciar atalho: " + (err.message || String(err));
      }
    }
  }

  document.addEventListener("keydown", function (event) {
    if (event.key === "Backspace" || event.keyCode === 10009) {
      event.preventDefault();
      try {
        if (window.tizen && tizen.application) {
          tizen.application.getCurrentApplication().exit();
        }
      } catch (_e) {}
    }
  });

  window.addEventListener("DOMContentLoaded", function () {
    setTimeout(launchMoonlight, 100);
  });
})();
