#include <iostream>
#include <string>
#include <amqpcpp.h>
#include <amqpcpp/libboostasio.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

// Callback per ricevere i dati di risposta HTTP
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Funzione per creare una sessione di pagamento Stripe
std::string createStripePaymentSession(const std::string& secretKey, double amount, const std::string& currency) {
    CURL* curl = curl_easy_init();
    std::string readBuffer;

    if (curl) {
        std::string url = "https://api.stripe.com/v1/checkout/sessions";

        // Corpo della richiesta
        nlohmann::json paymentJson = {
            {"payment_method_types", {"card"}},
            {"line_items", {{
                {"price_data", {
                    {"currency", currency},
                    {"product_data", {{"name", "Product"}}},
                    {"unit_amount", static_cast<int>(amount * 100)} // Importo in centesimi
                }},
                {"quantity", 1}
            }}},
            {"mode", "payment"},
            {"success_url", "https://example.com/success"},
            {"cancel_url", "https://example.com/cancel"}
        };

        std::string jsonData = paymentJson.dump();

        // Intestazioni della richiesta
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");
        headers = curl_slist_append(headers, ("Authorization: Bearer " + secretKey).c_str());

        // Configurazione di CURL
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "Errore nel pagamento Stripe: " << curl_easy_strerror(res) << std::endl;
        }

        curl_easy_cleanup(curl);
    }

    return readBuffer;
}

// Funzione principale per la gestione di RabbitMQ
void processRabbitMQ() {
    boost::asio::io_context io_context;

    // Configurazione della connessione RabbitMQ
    AMQP::Address address("amqp://guest:guest@localhost/");
    AMQP::LibBoostAsioHandler handler(io_context);
    AMQP::TcpConnection connection(&handler, address);
    AMQP::TcpChannel channel(&connection);

    std::string inputQueue = "stripePaymentQueue";
    std::string outputQueue = "stripeResponseQueue";

    // Dichiarazione delle code
    channel.declareQueue(inputQueue);
    channel.declareQueue(outputQueue);

    // Consuma i messaggi dalla coda di input
    channel.consume(inputQueue).onReceived([&](const AMQP::Message& message, uint64_t deliveryTag, bool redelivered) {
        std::string body(message.body(), message.bodySize());
        auto inputData = nlohmann::json::parse(body);

        std::string secretKey = inputData["secret_key"];
        double amount = inputData["amount"];
        std::string currency = inputData["currency"];

        std::string paymentResponse = createStripePaymentSession(secretKey, amount, currency);

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
