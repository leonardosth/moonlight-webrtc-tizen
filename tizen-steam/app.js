(function () {
  "use strict";

  const TARGET_APP_ID = "MlWrtcTst1.MoonlightWebRTCTest";
  const statusElement = document.getElementById("status-message");
  const spinnerElement = document.getElementById("spinner");

  function showError(msg) {
    if (spinnerElement) {
      spinnerElement.style.display = "none";
    }
    if (statusElement) {
      statusElement.className = "status-message error-message";
      statusElement.textContent = msg;
    }
  }

  function setStatus(msg) {
    if (statusElement) {
      statusElement.textContent = msg;
    }
  }

  function launchMoonlight() {
    if (!window.tizen || !tizen.application) {
      showError("Ambiente Tizen não detectado (executando no navegador).");
      return;
    }

    setStatus("Iniciando Moonlight WebRTC...");

    try {
      const appControlData = new tizen.ApplicationControlData("autostart", ["Steam"]);
      const appControl = new tizen.ApplicationControl(
        "http://tizen.org/appcontrol/operation/view",
        "steam",
        null,
        null,
        [appControlData]
      );

      tizen.application.launchAppControl(
        appControl,
        TARGET_APP_ID,
        function () {
          setStatus("Conectando ao Moonlight...");
          setTimeout(function () {
            try {
              tizen.application.getCurrentApplication().exit();
            } catch (_e) {}
          }, 800);
        },
        function (error) {
          console.warn("launchAppControl falhou, tentando launch direto:", error);
          try {
            tizen.application.launch(
              TARGET_APP_ID,
              function () {
                setStatus("Conectando ao Moonlight...");
                setTimeout(function () {
                  try {
                    tizen.application.getCurrentApplication().exit();
                  } catch (_e) {}
                }, 800);
              },
              function (launchErr) {
                const errDetail = (launchErr && (launchErr.message || launchErr.name)) || (error && (error.message || error.name)) || "App não encontrado";
                showError("Erro ao abrir Moonlight (" + errDetail + "). Verifique se o Moonlight WebRTC Client está instalado na TV.");
              }
            );
          } catch (e) {
            showError("Erro ao abrir Moonlight: " + (e.message || String(e)));
          }
        }
      );
    } catch (err) {
      console.error("Erro ao preparar AppControl:", err);
      try {
        tizen.application.launch(
          TARGET_APP_ID,
          function () {
            setStatus("Conectando ao Moonlight...");
            setTimeout(function () {
              try {
                tizen.application.getCurrentApplication().exit();
              } catch (_e) {}
            }, 800);
          },
          function (launchErr) {
            showError("Erro ao abrir Moonlight: " + (launchErr.message || err.message));
          }
        );
      } catch (e) {
        showError("Erro ao preparar atalho: " + (err.message || String(err)));
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
