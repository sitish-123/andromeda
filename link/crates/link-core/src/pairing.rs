//! Drives `link_proto::pairing` over a control stream, after both hellos.

use link_proto::message::{Message, PairConfirm, PairMethod, PairSpake};
use link_proto::pairing::{self, ClientPairing, EXPORTER_LABEL, EXPORTER_LEN, Secret, Secrets, ServerPairing};

use crate::Error;
use crate::control::Control;
use crate::identity::Spki;

pub async fn pair_as_client(
    connection: &quinn::Connection,
    control: &mut Control,
    secret: &Secret,
    method: PairMethod,
    own: &Spki,
) -> Result<(), Error> {
    let server = crate::tls::peer_spki(connection.peer_identity())?;
    let transcript = pairing::transcript(&exporter(connection)?, server.as_der(), own.as_der());
    let (pairing, spake) = ClientPairing::start(secret, method, transcript);
    control.send(Message::PairSpake(spake)).await?;
    let (confirming, confirm) = pairing.on_server_spake(&expect_spake(control.recv().await?)?)?;
    control.send(Message::PairConfirm(confirm)).await?;
    Ok(confirming.on_server_confirm(&expect_confirm(control.recv().await?)?)?)
}

pub async fn pair_as_server(
    connection: &quinn::Connection,
    control: &mut Control,
    secrets: &Secrets,
    own: &Spki,
    client: &Spki,
) -> Result<(), Error> {
    let transcript = pairing::transcript(&exporter(connection)?, own.as_der(), client.as_der());
    let (confirming, spake) =
        ServerPairing::new(transcript).on_client_spake(secrets, &expect_spake(control.recv().await?)?)?;
    control.send(Message::PairSpake(spake)).await?;
    let confirm = confirming.on_client_confirm(&expect_confirm(control.recv().await?)?)?;
    control.send(Message::PairConfirm(confirm)).await?;
    Ok(())
}

fn exporter(connection: &quinn::Connection) -> Result<[u8; EXPORTER_LEN], Error> {
    let mut out = [0; EXPORTER_LEN];
    connection.export_keying_material(&mut out, EXPORTER_LABEL, &[]).map_err(|_| Error::BadKey)?;
    Ok(out)
}

fn expect_spake(message: Message) -> Result<PairSpake, Error> {
    match message {
        Message::PairSpake(spake) => Ok(spake),
        other => Err(Error::Unexpected(other.kind())),
    }
}

fn expect_confirm(message: Message) -> Result<PairConfirm, Error> {
    match message {
        Message::PairConfirm(confirm) => Ok(confirm),
        other => Err(Error::Unexpected(other.kind())),
    }
}
