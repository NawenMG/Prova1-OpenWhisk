#include <iostream>
#include <string>
#include <amqpcpp.h>
#include <amqpcpp/libboostasio.h>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>

// Funzione principale del router OpenWhisk
void processRouter() {
    boost::asio::io_context io_context;

    // Configurazione della connessione RabbitMQ
    AMQP::Address address("amqp://guest:guest@localhost/");
    AMQP::LibBoostAsioHandler handler(io_context);
    AMQP::TcpConnection connection(&handler, address);
    AMQP::TcpChannel channel(&connection);

    std::string inputQueue = "routerQueue";
    std::string paypalQueue = "paypalQueue";
    std::string stripeQueue = "stripeQueue";
    std::string outputQueue = "routerResponseQueue";

    // Dichiarazione delle code
    channel.declareQueue(inputQueue);
    channel.declareQueue(paypalQueue);
    channel.declareQueue(stripeQueue);
    channel.declareQueue(outputQueue);

    // Consuma i messaggi dalla coda di input
    channel.consume(inputQueue).onReceived([&](const AMQP::Message& message, uint64_t deliveryTag, bool redelivered) {
        try {
            std::string body(message.body(), message.bodySize());
            auto inputData = nlohmann::json::parse(body);

            if (!inputData.contains("type")) {
                throw std::runtime_error("Il campo 'type' non è specificato.");
            }

            std::string type = inputData["type"];
            std::string response;

            if (type == "paypal") {
                // Pubblica il messaggio nella coda di PayPal
                channel.publish("", paypalQueue, body);
                response = R"({"status":"success","gateway":"paypal"})";
            } else if (type == "stripe") {
                // Pubblica il messaggio nella coda di Stripe
                channel.publish("", stripeQueue, body);
                response = R"({"status":"success","gateway":"stripe"})";
            } else {
                throw std::runtime_error("Tipo di pagamento sconosciuto: " + type);
            }

            // Pubblica la risposta nella coda di output
            channel.publish("", outputQueue, response);
            channel.ack(deliveryTag);

        } catch (const std::exception& e) {
            std::cerr << "Errore nel router: " << e.what() << std::endl;

            // Pubblica l'errore nella coda di output
            std::string errorResponse = R"({"status":"error","message":")" + std::string(e.what()) + R"("})";
            channel.publish("", outputQueue, errorResponse);
            channel.ack(deliveryTag);
        }
    });

    std::cout << "Router in attesa di messaggi su " << inputQueue << "..." << std::endl;
    io_context.run();
}

int main() {
    processRouter();
    return 0;
}
