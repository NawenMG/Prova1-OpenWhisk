#include <iostream>
#include <string>
#include <amqpcpp.h>
#include <amqpcpp/libboostasio.h>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

// Funzione principale del router per la gestione delle spedizioni
void processShippingRouter() {
    boost::asio::io_context io_context;

    // Configurazione della connessione RabbitMQ
    AMQP::Address address("amqp://guest:guest@localhost/");
    AMQP::LibBoostAsioHandler handler(io_context);
    AMQP::TcpConnection connection(&handler, address);
    AMQP::TcpChannel channel(&connection);

    std::string inputQueue = "shippingRouterQueue";
    std::string upsQueue = "upsShippingQueue";
    std::string fedexQueue = "fedexShippingQueue";
    std::string dhlQueue = "dhlShippingQueue";
    std::string outputQueue = "shippingRouterResponseQueue";

    // Dichiarazione delle code
    channel.declareQueue(inputQueue);
    channel.declareQueue(upsQueue);
    channel.declareQueue(fedexQueue);
    channel.declareQueue(dhlQueue);
    channel.declareQueue(outputQueue);

    // Consuma i messaggi dalla coda di input
    channel.consume(inputQueue).onReceived([&](const AMQP::Message& message, uint64_t deliveryTag, bool redelivered) {
        try {
            std::string body(message.body(), message.bodySize());
            auto inputData = nlohmann::json::parse(body);

            if (!inputData.contains("carrier")) {
                throw std::runtime_error("Il campo 'carrier' non è specificato.");
            }

            std::string carrier = inputData["carrier"];
            std::string response;

            // Inoltra alla coda corrispondente in base al vettore
            if (carrier == "ups") {
                channel.publish("", upsQueue, body);
                response = R"({"status":"success","carrier":"ups"})";
            } else if (carrier == "fedex") {
                channel.publish("", fedexQueue, body);
                response = R"({"status":"success","carrier":"fedex"})";
            } else if (carrier == "dhl") {
                channel.publish("", dhlQueue, body);
                response = R"({"status":"success","carrier":"dhl"})";
            } else {
                throw std::runtime_error("Carrier sconosciuto: " + carrier);
            }

            // Pubblica la risposta nella coda di output
            channel.publish("", outputQueue, response);
            channel.ack(deliveryTag);

        } catch (const std::exception& e) {
            std::cerr << "Errore nel router delle spedizioni: " << e.what() << std::endl;

            // Pubblica l'errore nella coda di output
            std::string errorResponse = R"({"status":"error","message":")" + std::string(e.what()) + R"("})";
            channel.publish("", outputQueue, errorResponse);
            channel.ack(deliveryTag);
        }
    });

    std::cout << "Router per le spedizioni in attesa di messaggi su " << inputQueue << "..." << std::endl;
    io_context.run();
}

int main() {
    processShippingRouter();
    return 0;
}
