#include <iostream>
#include <string>
#include <amqpcpp.h>
#include <amqpcpp/libboostasio.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

// Callback per la risposta HTTP
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Funzione per ottenere un token PayPal
std::string getPaypalToken(const std::string& clientId, const std::string& clientSecret) {
    CURL* curl = curl_easy_init();
    std::string readBuffer;

    if (curl) {
        std::string url = "https://api.sandbox.paypal.com/v1/oauth2/token";
        std::string auth = clientId + ":" + clientSecret;

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Accept: application/json");
        headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_USERPWD, auth.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "grant_type=client_credentials");
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "Errore nel token PayPal: " << curl_easy_strerror(res) << std::endl;
            curl_easy_cleanup(curl);
            return "";
        }

        curl_easy_cleanup(curl);
    }

    auto jsonResponse = nlohmann::json::parse(readBuffer);
    return jsonResponse["access_token"].get<std::string>();
}

// Funzione per effettuare un pagamento PayPal
std::string makePayment(const std::string& token, double amount, const std::string& currency) {
    CURL* curl = curl_easy_init();
    std::string readBuffer;

    if (curl) {
        std::string url = "https://api.sandbox.paypal.com/v1/payments/payment";

        nlohmann::json paymentJson = {
            {"intent", "sale"},
            {"payer", {{"payment_method", "credit_card"}}},
            {"transactions", {{
                {"amount", {{"total", std::to_string(amount)}, {"currency", currency}}},
                {"description", "Pagamento per il prodotto"}
            }}}
        };

        std::string jsonData = paymentJson.dump();

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + token).c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "Errore nel pagamento PayPal: " << curl_easy_strerror(res) << std::endl;
            curl_easy_cleanup(curl);
            return "";
        }

        curl_easy_cleanup(curl);
    }

    return readBuffer;
}

// Gestione RabbitMQ per l'elaborazione dei messaggi
void processRabbitMQ() {
    boost::asio::io_context io_context;

    AMQP::Address address("amqp://guest:guest@localhost/");
    AMQP::LibBoostAsioHandler handler(io_context);
    AMQP::TcpConnection connection(&handler, address);
    AMQP::TcpChannel channel(&connection);

    std::string inputQueue = "paymentQueue";
    std::string outputQueue = "paymentResponseQueue";

    // Dichiarazione delle code
    channel.declareQueue(inputQueue);
    channel.declareQueue(outputQueue);

    // Consumare i messaggi dalla coda di input
    channel.consume(inputQueue).onReceived([&](const AMQP::Message& message, uint64_t deliveryTag, bool redelivered) {
        std::string body(message.body(), message.bodySize());
        auto inputData = nlohmann::json::parse(body);

        std::string clientId = inputData["client_id"];
        std::string clientSecret = inputData["client_secret"];
        double amount = inputData["amount"];
        std::string currency = inputData["currency"];

        std::string token = getPaypalToken(clientId, clientSecret);
        std::string paymentResponse;

        if (!token.empty()) {
            paymentResponse = makePayment(token, amount, currency);
        } else {
            paymentResponse = "{\"status\":\"error\", \"message\":\"Token non disponibile\"}";
        }

        // Pubblica la risposta nella coda di output
        channel.publish("", outputQueue, paymentResponse);
        channel.ack(deliveryTag);
    });

    std::cout << "In attesa di messaggi su " << inputQueue << "..." << std::endl;
    io_context.run();
}

int main() {
    processRabbitMQ();
    return 0;
}
