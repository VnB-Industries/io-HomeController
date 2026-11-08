(function () {
    function ensureJson(response) {
        return response.json().catch(function () {
            return {};
        });
    }

    async function requestJson(url, options) {
        const requestOptions = options || {};
        const method = (requestOptions.method || "GET").toUpperCase();
        const requestUrl = method === "GET"
            ? url + (url.indexOf("?") === -1 ? "?" : "&") + "_=" + Date.now()
            : url;

        if (method === "GET") {
            requestOptions.cache = "no-store";
        }

        // The board answers 503 when its memory is low (load shedding); the
        // request was refused before any work, so it is safe to send again.
        let response;
        for (let attempt = 0; ; attempt++) {
            try {
                response = await fetch(requestUrl, requestOptions);
            } catch (error) {
                throw new Error("Device unreachable. Check that it's online or hasn't just restarted.");
            }
            if (response.status !== 503 || attempt >= 4) {
                break;
            }
            await new Promise(function (resolve) {
                setTimeout(resolve, 400 * (attempt + 1));
            });
        }
        const data = await ensureJson(response);
        if (!response.ok) {
            throw new Error(data.message || ("HTTP error " + response.status));
        }
        return data;
    }

    async function postJson(url, payload) {
        return requestJson(url, {
            method: "POST",
            headers: { "Content-Type": "application/json" },
            body: JSON.stringify(payload)
        });
    }

    async function uploadFile(url, file) {
        const formData = new FormData();
        formData.append("file", file);
        return requestJson(url, {
            method: "POST",
            body: formData
        });
    }

    async function downloadFile(url, filename) {
        const response = await fetch(url);
        if (!response.ok) {
            throw new Error("Network response was not ok");
        }

        const blob = await response.blob();
        const link = document.createElement("a");
        link.href = window.URL.createObjectURL(blob);
        link.download = filename;
        link.click();
        window.URL.revokeObjectURL(link.href);
    }

    window.MiOpenApi = {
        downloadFile: downloadFile,
        postJson: postJson,
        requestJson: requestJson,
        uploadFile: uploadFile
    };
})();
