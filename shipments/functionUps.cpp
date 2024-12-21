#include <iostream>
#include <string>
#include <amqpcpp.h>
#include <amqpcpp/libboostasio.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

// Callback per gestire la risposta HTTP
size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// Funzione per ottenere una stima delle tariffe di spedizione da UPS
std::string getUpsShippingQuote(
    const std::string& accessKey,
    const std::string& userId,
    const std::string& password,
    const std::string& originCountry,
    const std::string& destinationCountry,
    double weight, double length, double width, double height) {

    CURL* curl = curl_easy_init();
    std::string readBuffer;

    if (curl) {
        std::string url = "https://onlinetools.ups.com/rest/Rate";

        // Corpo della richiesta (JSON)
        nlohmann::json requestBody = {
            {"AccessRequest", {
                {"AccessLicenseNumber", accessKey},
                {"UserId", userId},
                {"Password", password}
            }},
            {"RateRequest", {
                {"Shipment", {
                    {"Shipper", {{"Address", {{"CountryCode", originCountry}}}}},
                    {"ShipTo", {{"Address", {{"CountryCode", destinationCountry}}}}},
                    {"Package", {{
                        {"PackagingType", {{"Code", "02"}}},
                        {"Dimensions", {
                            {"Length", length},
                            {"Width", width},
                            {"Height", height}
                        }},
                        {"PackageWeight", {
                            {"UnitOfMeasurement", {{"Code", "LBS"}}},
                            {"Weight", weight}
                        }}
                    }}
                }}
            }}
        }};

        std::string jsonData = requestBody.dump();

        // Intestazioni HTTP
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "Content-Type: application/json");

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonData.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "Errore CURL: " << curl_easy_strerror(res) << std::endl;
            curl_easy_cleanup(curl);
            return R"({"status":"error","message":"Errore nella richiesta HTTP"})";
        }

        curl_easy_cleanup(curl);
    }

    return readBuffer;
}

// Gestione RabbitMQ per la ricezione e l'elaborazione delle richieste
void processRabbitMQ() {
    boost::asio::io_context io_context;

    AMQP::Address address("amqp://guest:guest@localhost/");
    AMQP::LibBoostAsioHandler handler(io_context);
    AMQP::TcpConnection connection(&handler, address);
    AMQP::TcpChannel channel(&connection);

    std::string inputQueue = "upsShippingQueue";
    std::string outputQueue = "upsShippingResponseQueue";

    // Dichiarazione delle code
    channel.declareQueue(inputQueue);
    channel.declareQueue(outputQueue);

    // Consuma i messaggi dalla coda di input
    channel.consume(inputQueue).onReceived([&](const AMQP::Message& message, uint64_t deliveryTag, bool redelivered) {
        try {
            std::string body(message.body(), message.bodySize());
            auto inputData = nlohmann::json::parse(body);

            // Parametri richiesti
            std::string accessKey = inputData.at("access_key");
            std::string userId = inputData.at("user_id");
            std::string password = inputData.at("password");
            std::string originCountry = inputData.at("origin_country");
            std::string destinationCountry = inputData.at("destination_country");
            double weight = inputData.at("weight");
            double length = inputData.at("length");
            double width = inputData.at("width");
            double height = inputData.at("height");

            // Ottenere il preventivo di spedizione
            std::string shippingQuoteResponse = getUpsShippingQuote(accessKey, userId, password, originCountry, destinationCountry, weight, length, width, height);

            // Pubblica la risposta nella coda di output
            channel.publish("", outputQueue, shippingQuoteResponse);
            channel.ack(deliveryTag);

        } catch (const std::exception& e) {
            std::cerr << "Errore nella gestione del messaggio: " << e.what() << std::endl;

            // Invia una risposta di errore
            std::string errorResponse = R"({"status":"error","message":")" + std::string(e.what()) + R"("})";
            channel.publish("", outputQueue, errorResponse);
            channel.ack(deliveryTag);
        }
    });

    std::cout << "In attesa di messaggi sulla coda " << inputQueue << "..." << std::endl;
    io_context.run();
}

int main() {
    processRabbitMQ();
    return 0;
}
